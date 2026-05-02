# BriskNet — System Architecture

## 1. Overview

BriskNet is a **user-space packet processing pipeline** built on top of AF_XDP, designed for **low-latency and deterministic packet handling**.

The system bypasses the traditional Linux networking stack by redirecting packets at the XDP layer directly into user-space memory (UMEM), where they are processed with minimal overhead through a multi-threaded, lock-free pipeline.

### Core Objective

* Minimize latency (p50 / p99 / p999)
* Maintain deterministic processing
* Reduce kernel involvement in the data path
* Provide a clean, extensible pipeline for future TCP/IP and HFT-style systems

---

## 2. End-to-End Data Flow

```text
NIC
 ↓
XDP (eBPF program)
 ↓
XSKMAP (queue → socket mapping)
 ↓
AF_XDP socket
 ↓
UMEM (DMA-backed memory)
 ↓
RX ring
 ↓
Poller thread (CPU 2) — batch peek, prefetch, encode addr+len
 ↓
Lock-free SPSC queue (LFQueue, 8192 entries)
 ↓
Worker thread (CPU 3) — bulk dequeue, parse, process, batch-recycle
 ↓
Fill ring (buffer recycling)
```

---

## 3. Core Components

### 3.1 XDP Program (`bpf/xdp_redirect.bpf.c`)

The XDP program runs in kernel space and executes **before the kernel networking stack**.

#### Responsibilities:

* Receive packet at NIC ingress
* Identify RX queue (`ctx->rx_queue_index`)
* Redirect packet using `bpf_redirect_map()`

#### Key mechanism:

```c
return bpf_redirect_map(&xsks_map, index, 0);
```

This ensures packets are **diverted into AF_XDP sockets instead of the kernel stack**.

---

### 3.2 XSKMAP

A BPF map of type:

```text
BPF_MAP_TYPE_XSKMAP
```

#### Purpose:

Maps:

```text
queue_id → AF_XDP socket FD
```

#### Role in pipeline:

* Enables kernel → userspace redirection
* Determines which socket receives packets from which NIC queue

---

### 3.3 UMEM (`src/umem.c`, `include/umem.h`)

UMEM is a **pre-allocated, DMA-compatible memory region** shared between kernel and user space.

#### Layout:

```text
UMEM (8 MB total)
├── Frame 0    [2048 bytes]
├── Frame 1    [2048 bytes]
├── Frame 2    [2048 bytes]
...
├── Frame 4095 [2048 bytes]
```

#### Properties:

* 4096 fixed-size frames (2048 bytes each)
* Allocated via `mmap` with `MAP_POPULATE` (pre-faults pages)
* Alignment enforced via `_Static_assert`
* Directly written by NIC (via DMA)

#### Key responsibilities:

* Eliminate copies
* Provide predictable memory layout
* Enable zero/low-copy packet access

---

### 3.4 Rings (Lockless Queues)

BriskNet uses AF_XDP's shared ring structures:

#### Fill Ring (Userspace → Kernel)

* Provides empty buffers for incoming packets
* **Owned by the worker thread** (batch-recycled after processing)
* Batch reserve/submit for reduced overhead

#### RX Ring (Kernel → Userspace)

* Kernel publishes received packets
* Contains descriptors with:
  * `addr` (offset in UMEM)
  * `len` (packet length)
* Consumed by poller thread in batches of up to 64

#### Completion Ring (TX path — future)

* Tracks transmitted buffers
* Currently unused

---

### 3.5 AF_XDP Socket (`src/af_xdp_socket.c`, `include/af_xdp_socket.h`)

Created via:

```c
xsk_socket__create(...)
```

#### Responsibilities:

* Bind UMEM to NIC queue
* Connect XDP layer with user-space
* Expose RX/TX rings

#### Configuration:

* SKB mode (WiFi compatibility)
* NEED_WAKEUP enabled
* Single queue (queue_id = 0)
* Multi-queue ready (`queue_id` field in struct)

---

### 3.6 Lock-Free Queue (`external/lfqueue/`, `src/lfqueue_wrapper.cpp`, `include/lfqueue_wrapper.h`)

Inter-thread communication uses a **C++ SPSC lock-free ring buffer** (LFQueue) exposed via a C wrapper.

#### Properties:

* Template: `lockfree::Ring<uint64_t, SingleProducer=true, SingleConsumer=true>`
* Size: 8192 entries (power of 2)
* Cache-line aligned producer/consumer state
* Batch publish with explicit `flush()` for visibility control

#### Payload encoding:

```c
/* Upper 32 bits → addr, lower 32 bits → len */
#define PKT_ENCODE(addr, len) (((uint64_t)(addr) << 32) | (uint32_t)(len))
#define PKT_ADDR(val)         ((val) >> 32)
#define PKT_LEN(val)          ((uint32_t)((val) & 0xFFFFFFFF))
```

Single `uint64_t` per packet — no structs, no extra allocations.

#### C API:

* `lfqueue_create` / `lfqueue_destroy`
* `lfqueue_enqueue` / `lfqueue_dequeue`
* `lfqueue_enqueue_bulk` / `lfqueue_dequeue_bulk`
* `lfqueue_flush`
* `lfqueue_size` (approximate depth snapshot)

---

### 3.7 Packet Parser (`src/packet_parser.c`, `include/packet_parser.h`)

Parses raw Ethernet frames into structured protocol data.

#### Supported protocols:

* Ethernet → IPv4 → UDP
* Ethernet → IPv4 → TCP

#### Output (`struct parsed_packet`):

* Protocol type (UDP / TCP / OTHER)
* Source/destination IP and port
* TCP flags, sequence/ack numbers
* Payload pointer and length

---

### 3.8 TCP Flow Tracking (`include/tcp_flow.h`)

Lightweight connection state tracking in the hot path.

#### Properties:

* Fixed-size table (1024 flows, modulo-indexed)
* No dynamic allocation
* Detects: SYN, FIN, RST, ACK events
* Tracks connection state transitions

---

## 4. Pipeline Architecture

### 4.1 Two-Thread Model

```text
┌─────────────────────────────┐   ┌─────────────────────────────────┐
│       Poller (CPU 2)        │   │         Worker (CPU 3)           │
├─────────────────────────────┤   ├─────────────────────────────────┤
│ peek RX ring (batch 64)     │   │ dequeue_bulk from LFQueue       │
│ prefetch next desc + data   │   │ prefetch next packet buffer     │
│ encode addr+len → uint64_t  │   │ decode addr+len                 │
│ enqueue to LFQueue          │   │ parse_packet (ETH/IP/UDP/TCP)   │
│ flush queue                 │   │ latency measurement             │
│ release RX descriptors      │   │ TCP flow tracking               │
│ update rx/enq/drop counters │   │ batch-recycle buffers to fill   │
└─────────────────────────────┘   │ update processed counter        │
                                  │ print stats every 1s            │
                                  └─────────────────────────────────┘
```

### 4.2 CPU Pinning

* Poller → CPU 2 (`pthread_setaffinity_np`)
* Worker → CPU 3
* Eliminates scheduling jitter and cross-core cache thrashing

### 4.3 Buffer Ownership

```text
Fill Ring → Kernel owns buffer (waiting for packet)
RX Ring   → Poller reads descriptor, enqueues to worker
Worker    → Owns buffer during processing
Fill Ring → Worker recycles buffer back to kernel
```

Critical: only the worker recycles buffers (after processing completes).

---

## 5. Performance Optimizations

### 5.1 Batch Processing

* **Poller**: peeks up to 64 descriptors per iteration
* **Worker**: bulk-dequeues up to 64 items from LFQueue
* **Buffer recycling**: single `reserve` + loop fill + single `submit` per batch

### 5.2 CPU Prefetching

* **Poller**: prefetches next RX descriptor (`locality=1`) and next UMEM packet data (`locality=3`) one step ahead
* **Worker**: prefetches next packet's UMEM buffer one step ahead before processing current

### 5.3 Lock-Free Communication

* SPSC queue with no locks, no atomics in the fast path (batch publish)
* `lfqueue_flush()` called once per RX batch (amortized visibility cost)
* Relaxed-ordering atomic counters for observability (no memory barriers on hot path)

### 5.4 Zero-Copy Payload Encoding

* Addr + len packed into single `uint64_t` — shift + OR to encode, shift + mask to decode
* No struct copies across the queue boundary

---

## 6. Observability

### 6.1 Atomic Counters (lock-free)

| Counter | Owner | Description |
|---------|-------|-------------|
| `packets_received` | Poller | Total packets consumed from RX ring |
| `packets_enqueued` | Poller | Successfully enqueued to LFQueue |
| `packets_dropped` | Poller | Dropped (queue full) |
| `packets_processed` | Worker | Successfully processed |

### 6.2 Queue Depth

* `lfqueue_size()` — reads `prod_tail - cons_tail` (approximate, O(1))

### 6.3 Latency Distribution

* **Ring buffer**: 1M-entry preallocated `uint64_t` array (no malloc in hot path)
* **Measurement**: embedded nanosecond timestamp in UDP payload (sender → receiver)
* **Percentiles**: quickselect algorithm (O(n) average) computed every second
* **Reported**: avg, p50, p99, p999, max


## 7. Project Structure

```text
BriskNet/
├── bpf/
│   └── xdp_redirect.bpf.c       # XDP/eBPF redirect program
├── external/
│   └── lfqueue/                  # Git submodule: lock-free SPSC queue (C++)
├── include/
│   ├── af_xdp_socket.h          # XSK socket struct + creation API
│   ├── lfqueue_wrapper.h         # C API for LFQueue
│   ├── packet_parser.h           # Protocol parsing interface
│   ├── tcp_flow.h                # TCP flow table + event detection
│   └── umem.h                    # UMEM config + frame layout
├── src/
│   ├── af_xdp_socket.c           # XSK socket creation + BPF map registration
│   ├── lfqueue_wrapper.cpp       # C++ → C bridge for LFQueue
│   ├── main.c                    # Pipeline orchestration (poller + worker)
│   ├── packet_parser.c           # Ethernet/IPv4/UDP/TCP parsing
│   └── umem.c                    # UMEM allocation via mmap
├── benchmark/
│   ├── udp_sender.c              # UDP packet generator with embedded timestamp
│   ├── kernel_udp_receiver.c     # Baseline kernel-socket receiver
│   └── results.txt               # Runtime CSV output
├── scripts/
│   └── run_benchmark.sh          # Automated benchmark runner
├── Makefile
└── architecture.md
```

---

## 9. Memory Ownership Model

```text
Startup:     All frames → Fill Ring (kernel owns)
Packet RX:   Kernel writes to frame → RX ring descriptor published
Poller:      Reads descriptor, encodes addr+len, enqueues to LFQueue
Worker:      Decodes, accesses UMEM buffer, processes packet
Recycle:     Worker batch-submits addrs back to Fill Ring → kernel regains ownership
```

---

## 10. Current Limitations

### Hardware

* Running on WiFi NIC (`lo`)
* Uses `XDP_SKB` mode (no zero-copy DMA)
* Higher latency than native/offload mode

### Software

* Single RX queue (multi-queue RSS ready but not active)
* No TX path
* Fixed flow table size (1024 entries, modulo-indexed)
* Quickselect percentile computation runs in worker thread (brief pause each second)

---

## 11. Design Philosophy

### Minimalism

* Small codebase, no unnecessary abstractions
* Single `uint64_t` carries full packet metadata across threads

### Determinism

* CPU-pinned threads eliminate scheduling jitter
* Lock-free queue with bounded capacity (drops over blocking)
* Preallocated buffers throughout (no malloc in hot path)

### Explicit Control

* No hidden behavior
* Manual memory, ring, and thread management
* Poller never blocks; worker never blocks on dequeue

### Performance-first design

* Data movement optimized before abstractions
* Prefetch, batching, and cache-line alignment throughout
* Relaxed atomics for counters (no unnecessary barriers)

---

## 12. Future Work

* Multi-queue RSS scaling (one poller+worker pair per queue)
* TX path with completion ring
* Native/zero-copy XDP mode on wired NIC
* Scalable flow table (hash-based)
* HDR histogram for latency (constant-time percentiles)
* Custom packet actions (filtering, forwarding, load balancing)

---
