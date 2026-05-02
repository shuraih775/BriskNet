#!/bin/bash
set -e

MODE="${1:-kernel}"          # "kernel" or "xdp"
DURATION="${2:-10}"          # seconds to run
TARGET_IP="${3:-127.0.0.1}"
PORT="${4:-9000}"
PKT_SIZE="${5:-64}"

LOG_DIR="./benchmark/logs"
mkdir -p "$LOG_DIR"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RX_LOG="$LOG_DIR/${MODE}_rx_${TIMESTAMP}.log"
TX_LOG="$LOG_DIR/${MODE}_tx_${TIMESTAMP}.log"

#  Build 
echo "[*] building..."
if [ "$MODE" = "kernel" ]; then
    gcc -O2 benchmark/kernel_udp_receiver.c -o benchmark/kernel_udp_receiver
elif [ "$MODE" = "xdp" ]; then
    gcc -O2 -c src/main.c src/af_xdp_socket.c src/umem.c src/packet_parser.c
    g++ -O2 -std=c++17 -c src/lfqueue_wrapper.cpp
    g++ main.o af_xdp_socket.o umem.o packet_parser.o lfqueue_wrapper.o -o brisknet -lxdp -lbpf -lpthread
    rm -f *.o
else
    echo "usage: $0 [kernel|xdp] [duration] [target_ip] [port] [pkt_size]"
    exit 1
fi
gcc -O2 benchmark/udp_sender.c -o benchmark/udp_sender

echo "[*] starting $MODE receiver..."
if [ "$MODE" = "kernel" ]; then
    ./benchmark/kernel_udp_receiver > "$RX_LOG" 2>&1 &
else
    sudo ./brisknet > "$RX_LOG" 2>&1 &
fi
RX_PID=$!

sleep 1  # let receiver bind

echo "[*] starting sender -> $TARGET_IP:$PORT (pkt_size=$PKT_SIZE)"
./benchmark/udp_sender "$TARGET_IP" "$PORT" "$PKT_SIZE" > "$TX_LOG" 2>&1 &
TX_PID=$!

echo "[*] running for ${DURATION}s..."
sleep "$DURATION"

#  Stop 
echo "[*] stopping..."
kill "$TX_PID" 2>/dev/null || true
kill "$RX_PID" 2>/dev/null || true
wait "$TX_PID" 2>/dev/null || true
wait "$RX_PID" 2>/dev/null || true

#  Results 
echo ""
echo " Receiver output (last 5 lines) "
tail -5 "$RX_LOG"
echo ""
echo " Sender output (last 5 lines) "
tail -5 "$TX_LOG"
echo ""
echo "[*] logs saved:"
echo "    RX: $RX_LOG"
echo "    TX: $TX_LOG"
