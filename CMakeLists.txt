cmake_minimum_required(VERSION 3.16)
project(BriskNet LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O2")

# --- Main binary ---
add_executable(brisknet
    src/main.c
    src/af_xdp_socket.c
    src/umem.c
    src/packet_parser.c
    src/lfqueue_wrapper.cpp
)

target_include_directories(brisknet PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/external/lfqueue/include
)

target_link_libraries(brisknet PRIVATE xdp bpf pthread)

# --- Benchmark: UDP sender ---
add_executable(udp_sender benchmark/udp_sender.c)

# --- Benchmark: kernel UDP receiver ---
add_executable(kernel_udp_receiver benchmark/kernel_udp_receiver.c)

# --- Install targets into build/ for convenience ---
set_target_properties(brisknet udp_sender kernel_udp_receiver
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
