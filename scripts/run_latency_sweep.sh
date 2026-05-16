#!/bin/bash
export PATH=$HOME/local/bin:$PATH
export LD_LIBRARY_PATH=$HOME/local/lib:$LD_LIBRARY_PATH

cd ~/lustre/TopoSteal

echo "=== Config A: Current (2400 tasks, 500K iters, 2 prod) ==="
sed -i 's/#define NUM_TASKS.*/#define NUM_TASKS      2400/' bench/latency_bench.c
sed -i 's/#define CHASE_ITERS.*/#define CHASE_ITERS    500000/' bench/latency_bench.c
sed -i 's/#define PRODUCERS_PER_NODE.*/#define PRODUCERS_PER_NODE 2/' bench/latency_bench.c
make bench_latency
./bench_latency 2>&1 | tee latency_A.txt

echo ""
echo "=== Config B: Heavy DRAM (1200 tasks, 2M iters, 1 prod) ==="
sed -i 's/#define NUM_TASKS.*/#define NUM_TASKS      1200/' bench/latency_bench.c
sed -i 's/#define CHASE_ITERS.*/#define CHASE_ITERS    2000000/' bench/latency_bench.c
sed -i 's/#define PRODUCERS_PER_NODE.*/#define PRODUCERS_PER_NODE 1/' bench/latency_bench.c
make bench_latency
./bench_latency 2>&1 | tee latency_B.txt

echo ""
echo "=== Config C: HFT burst (4800 tasks, 100K iters, 1 prod) ==="
sed -i 's/#define NUM_TASKS.*/#define NUM_TASKS      4800/' bench/latency_bench.c
sed -i 's/#define CHASE_ITERS.*/#define CHASE_ITERS    100000/' bench/latency_bench.c
sed -i 's/#define PRODUCERS_PER_NODE.*/#define PRODUCERS_PER_NODE 1/' bench/latency_bench.c
make bench_latency
./bench_latency 2>&1 | tee latency_C.txt

echo ""
echo "=== ALL DONE ==="
