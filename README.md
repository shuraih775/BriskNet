# BriskNet

High-performance user-space packet processing pipeline using AF_XDP. Bypasses the Linux kernel networking stack for low-latency, deterministic packet handling.

## Features

- Kernel-bypass via XDP + AF_XDP sockets
- Two-thread pipeline (poller + worker) with CPU pinning
- Lock-free SPSC queue (LFQueue) for inter-thread communication
- Batch RX, batch buffer recycling, CPU prefetching
- Full protocol parsing (Ethernet/IPv4/UDP/TCP)
- TCP flow tracking with state detection
- Latency measurement with percentiles (p50, p99, p999)
- Zero malloc in hot path

## Prerequisites

- Linux (kernel 5.4+)
- `libxdp` and `libbpf` (development headers)
- `clang` (for BPF compilation)
- `cmake` (>= 3.16)
- `g++` (C++17 support)
- `bpftool` (for map pinning)

Install on Ubuntu/Debian:

```bash
sudo apt install cmake build-essential clang libbpf-dev libxdp-dev linux-tools-common
```

## Build

```bash
git clone --recurse-submodules https://github.com/shuraih775/BriskNet.git
cd BriskNet
make
```

This runs CMake under the hood and produces binaries in `build/bin/`.

## Setup

### 1. Compile

```bash
make bpf
```

### 2. Attach the XDP program and Pin the BPF map and detach it again(code itself attaches when ran)

```bash
make link
make pin
make unlink
```

### 4. Run BriskNet

```bash
make run
```

### 5. Unpin the map
```
once done running the code 
make link
make unpin
make unlink

```
Press `Ctrl+C` to stop. Stats are printed every second and saved to `benchmark/results.txt`.


## Benchmarking

Run a full benchmark (sender + receiver):

```bash
make bench MODE=xdp DUR=10 TARGET=127.0.0.1 PORT=9000 PKT=64
```

Run components individually:

```bash
# Terminal 1: start BriskNet
make run

# Terminal 2: fire UDP packets
make sender TARGET=127.0.0.1 PORT=9000 PKT=64
```

Kernel baseline comparison:

```bash
make bench MODE=kernel DUR=10
```

## All Make Commands

| Command | Description |
|---------|-------------|
| `make` | Build all targets |
| `make run` | Build + run BriskNet (sudo) |
| `make clean` | Remove build directory |
| `make bpf` | Compile XDP BPF program |
| `make link` | Attach XDP to NIC (lo) |
| `make unlink` | Detach XDP from NIC |
| `make pin` | Pin xsks_map to bpffs |
| `make bench` | Run full benchmark |
| `make sender` | Run UDP sender |
| `make receiver` | Run kernel UDP receiver |
| `make help` | Show all commands |

## Using as a Library

BriskNet components can be used independently in other projects:

### UMEM + AF_XDP socket

```c
#include "umem.h"
#include "af_xdp_socket.h"

struct umem_info umem;
umem_create(&umem);

struct xsk_socket_info *xsk;
xsk_socket_create(&xsk, &umem, "eth0", 0);

// Access RX ring, process packets, recycle buffers
```

### Packet parser

```c
#include "packet_parser.h"

struct parsed_packet pkt;
if (parse_packet(raw_data, len, &pkt) == 0) {
    // pkt.protocol, pkt.src_ip, pkt.dst_port, pkt.payload, etc.
}
```

### LFQueue (C wrapper)

```c
#include "lfqueue_wrapper.h"

lfqueue_t *q = lfqueue_create(8192);
lfqueue_enqueue(q, value);
lfqueue_flush(q);

uint64_t out;
lfqueue_dequeue(q, &out);
lfqueue_destroy(q);
```

### TCP flow tracking

```c
#include "tcp_flow.h"

struct tcp_flow_table flows;
tcp_flow_table_init(&flows);
enum tcp_event ev = tcp_flow_process(&flows, &parsed_pkt);
```

## Configuration

Key constants in `src/main.c`:

| Constant | Default | Description |
|----------|---------|-------------|
| `POLLER_CPU_CORE` | 2 | CPU core for poller thread |
| `WORKER_CPU_CORE` | 3 | CPU core for worker thread |
| `BATCH_SIZE` | 64 | Max packets per batch |
| `QUEUE_SIZE` | 8192 | LFQueue capacity |

NIC interface is set in `main()` — change `"lo"` to your interface.

## Architecture

For detailed design decisions, data flow diagrams, and performance optimization rationale, see [architecture.md](architecture.md).
