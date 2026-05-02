BUILD_DIR = build
BIN       = $(BUILD_DIR)/bin

.PHONY: all build run clean bpf link bench sender receiver help

#  Build 
all: build

build:
	@cmake -S . -B $(BUILD_DIR) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@cmake --build $(BUILD_DIR) -j$$(nproc)

#  Run 
run: build
	sudo $(BIN)/brisknet

#  Benchmarks 
sender: build
	$(BIN)/udp_sender $(TARGET) $(PORT) $(PKT)

receiver: build
	$(BIN)/kernel_udp_receiver

bench: build
	@bash scripts/run_benchmark.sh $(MODE) $(DUR) $(TARGET) $(PORT) $(PKT)

#  BPF 
bpf:
	clang -O2 -g -target bpf -c bpf/xdp_redirect.bpf.c -o bpf/xdp.o

link: unlink
	sudo ip link set dev lo xdp obj bpf/xdp.o sec xdp

unlink:
	sudo ip link set dev lo xdp off

pin: unpin
	sudo bpftool map pin name xsks_map /sys/fs/bpf/xsks_map

unpin:
	sudo rm -f /sys/fs/bpf/xsks_map

#  Clean 
clean:
	@rm -rf $(BUILD_DIR)
	@rm -f bpf/xdp.o
show-link:
	ip link show dev lo
#  Help 
help:
	@echo "BriskNet Makefile (CMake wrapper)"
	@echo ""
	@echo "  make              Build all targets"
	@echo "  make run          Build + run brisknet (sudo)"
	@echo "  make clean        Remove build directory"
	@echo ""
	@echo "  make bpf          Compile XDP program"
	@echo "  make link         Attach XDP to lo"
	@echo "  make unlink       Detach XDP from lo"
	@echo "  make pin          Pin xsks_map to bpffs"
	@echo ""
	@echo "  make bench MODE=xdp DUR=10 TARGET=127.0.0.1 PORT=9000 PKT=64"
	@echo "  make sender TARGET=127.0.0.1 PORT=9000 PKT=64"
	@echo "  make receiver"
	@echo ""

#  Defaults for optional params 
MODE   ?= xdp
DUR    ?= 10
TARGET ?= 127.0.0.1
PORT   ?= 9000
PKT    ?= 64