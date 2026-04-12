run:
	gcc ./src/*c -o main -lxdp -lbpf;./main

build-bpflib:
	clang -O2 -target bpf -c ./bpf/xdp_redirect.bpf.c -o ./bpf/xdp.o

linkup:
	sudo ip link set dev wlo1 xdp obj ./bpf/xdp.o sec xdp
pin-map:
	sudo bpftool map pin id 3 /sys/fs/bpf/xsk_map
	sudo bpftool map pin name xsks_map /sys/fs/bpf/xsks_map