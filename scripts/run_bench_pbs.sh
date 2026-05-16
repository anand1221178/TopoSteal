#!/bin/bash
#PBS -N toposteal_bench
#PBS -l select=1:ncpus=24:mpiprocs=1
#PBS -P DEVL1048
#PBS -q smp
#PBS -l walltime=0:30:00
#PBS -o toposteal_bench.out
#PBS -e toposteal_bench.err

echo "========================================================"
echo "TopoSteal Benchmark on $(hostname)"
echo "Job ID: $PBS_JOBID"
echo "Started at: $(date)"
echo "========================================================"

echo -e "\nCPU Info:"
lscpu | grep -E "Socket|NUMA|Core|Thread|CPU\(s\)|Model name"
echo -e "\nNUMA distances:"
numactl --hardware 2>/dev/null || cat /sys/devices/system/node/node*/distance 2>/dev/null || echo "No NUMA info"
echo -e "\nperf_event_paranoid:"
cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "unknown"

# Load hwloc module
module load chpc/comms/hwloc/1.11.3 2>/dev/null || true
echo -e "\nhwloc version:"
hwloc-info --version 2>/dev/null || echo "hwloc not found via module"

# If module hwloc is too old or missing, build from source
if ! command -v lstopo &>/dev/null; then
    echo "Building hwloc from source..."
    HWLOC_PREFIX="$HOME/local"
    cd /tmp
    rm -rf hwloc-2.9.3
    wget -q https://download.open-mpi.org/release/hwloc/v2.9/hwloc-2.9.3.tar.gz
    tar xzf hwloc-2.9.3.tar.gz
    cd hwloc-2.9.3
    ./configure --prefix="$HWLOC_PREFIX" --disable-cuda --disable-opencl --disable-nvml --quiet
    make -j$(nproc) && make install
    export PATH="$HWLOC_PREFIX/bin:$PATH"
    export LD_LIBRARY_PATH="$HWLOC_PREFIX/lib:$LD_LIBRARY_PATH"
    export C_INCLUDE_PATH="$HWLOC_PREFIX/include:$C_INCLUDE_PATH"
    export LIBRARY_PATH="$HWLOC_PREFIX/lib:$LIBRARY_PATH"
fi

echo -e "\nhwloc topology:"
lstopo --no-io --no-bridges --of console 2>/dev/null || hwloc-info 2>/dev/null || echo "lstopo unavailable"

# Build TopoSteal
echo -e "\n========================================================"
echo "Building TopoSteal..."
echo "========================================================"
CWD=/mnt/lustre/users/$USER/TopoSteal
cd "$CWD" || exit 1

# Make sure hwloc paths are set for compilation
if [ -d "$HOME/local/lib" ]; then
    export LD_LIBRARY_PATH="$HOME/local/lib:$LD_LIBRARY_PATH"
    export C_INCLUDE_PATH="$HOME/local/include:$C_INCLUDE_PATH"
    export LIBRARY_PATH="$HOME/local/lib:$LIBRARY_PATH"
fi

make clean && make
echo "Build complete."

# Run tests
echo -e "\n========================================================"
echo "Running parallel_sum..."
echo "========================================================"
./parallel_sum

echo -e "\n========================================================"
echo "Running stress test..."
echo "========================================================"
./deque_stress

# Run benchmark
echo -e "\n========================================================"
echo "Running pointer-chase benchmark..."
echo "========================================================"
./bench_pointer_chase

echo -e "\n========================================================"
echo "Job finished at: $(date)"
echo "========================================================"
