## Hardware Topology & Distance Matrix

TopoSteal uses `hwloc` to dynamically detect the physical layout of the CPU and encode it into a latency distance matrix. This ensures the scheduler understands the actual hardware boundaries (like P-core vs. E-core clusters) before making work-stealing decisions.

### Example: Apple Silicon (M-Series) Topology
When running `lstopo` on an M-series chip, we can see three distinct CPU clusters, each sharing its own separate L2 cache, with no shared L3 cache:

```text
Machine (3426MB total)
  Package L#0
    NUMANode L#0 (P#0 3426MB)
    L2 L#0 (4096KB)
      L1d L#0 (64KB) + L1i L#0 (128KB) + Core L#0 + PU L#0 (P#0)
      L1d L#1 (64KB) + L1i L#1 (128KB) + Core L#1 + PU L#1 (P#1)
      ...[Cores 2-3]
    L2 L#1 (16MB)
      L1d L#4 (128KB) + L1i L#4 (192KB) + Core L#4 + PU L#4 (P#4)
      ... [Cores 5-8]
    L2 L#2 (16MB)
      L1d L#9 (128KB) + L1i L#9 (192KB) + Core L#9 + PU L#9 (P#9)
      ... [Cores 10-13]
  CoProc(OpenCL) "opencl0d0"
```

### The Resulting Distance Matrix
TopoSteal parses this tree and generates the following distance matrix. Notice how the physical silicon clusters perfectly map to the distance penalties:

  Distance matrix (14 cores):
     C0   C1   C2   C3   C4   C5   C6   C7   C8   C9   C10  C11  C12  C13  
C0   0    1    1    1    8    8    8    8    8    8    8    8    8    8    
C1   1    0    1    1    8    8    8    8    8    8    8    8    8    8    
C2   1    1    0    1    8    8    8    8    8    8    8    8    8    8    
C3   1    1    1    0    8    8    8    8    8    8    8    8    8    8    
C4   8    8    8    8    0    1    1    1    1    8    8    8    8    8    
C5   8    8    8    8    1    0    1    1    1    8    8    8    8    8    
C6   8    8    8    8    1    1    0    1    1    8    8    8    8    8    
C7   8    8    8    8    1    1    1    0    1    8    8    8    8    8    
C8   8    8    8    8    1    1    1    1    0    8    8    8    8    8    
C9   8    8    8    8    8    8    8    8    8    0    1    1    1    1    
C10  8    8    8    8    8    8    8    8    8    1    0    1    1    1    
C11  8    8    8    8    8    8    8    8    8    1    1    0    1    1    
C12  8    8    8    8    8    8    8    8    8    1    1    1    0    1    
C13  8    8    8    8    8    8    8    8    8    1    1    1    1    0
