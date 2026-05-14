#!/bin/bash

#SBATCH --job-name=toposteal_bench
#SBATCH --partition=biggpu
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=112
#SBATCH --time=01:00:00
#SBATCH --exclusive
#SBATCH --output=toposteal_%j.out
#SBATCH --error=toposteal_%j.err

echo "========================================================"
echo "TopoSteal Benchmark on $(hostname)"
echo "Job ID: $SLURM_JOB_ID"
echo "Started at: $(date)"
echo "========================================================"

echo -e "\nCPU Info:"
lscpu | grep -E "Socket|NUMA|Core|Thread|CPU\(s\)|Model name"
echo -e "\nNUMA distances:"
cat /sys/devices/system/node/node*/distance 2>/dev/null || echo "No NUMA info in sysfs"

# Build hwloc from source
HWLOC_PREFIX="$HOME/local"
if [ ! -f "$HWLOC_PREFIX/lib/libhwloc.so" ]; then
    echo -e "\n========================================================"
    echo "Building hwloc from source..."
    echo "========================================================"
    cd /tmp
    wget -q https://download.open-mpi.org/release/hwloc/v2.9/hwloc-2.9.3.tar.gz
    tar xzf hwloc-2.9.3.tar.gz
    cd hwloc-2.9.3
    ./configure --prefix="$HWLOC_PREFIX" --quiet
    make -j16 && make install
    echo "hwloc installed to $HWLOC_PREFIX"
else
    echo -e "\nhwloc already installed at $HWLOC_PREFIX"
fi

export PATH="$HWLOC_PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$HWLOC_PREFIX/lib:$LD_LIBRARY_PATH"
export C_INCLUDE_PATH="$HWLOC_PREFIX/include:$C_INCLUDE_PATH"
export LIBRARY_PATH="$HWLOC_PREFIX/lib:$LIBRARY_PATH"

echo -e "\nhwloc topology:"
lstopo --no-io --no-bridges --of console

# Build TopoSteal
echo -e "\n========================================================"
echo "Building TopoSteal..."
echo "========================================================"
cd "$HOME/TopoSteal" || exit 1
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
