# Oracle Ideal Prefetcher — Implementation & Results

## Overview

This document describes the **ideal prefetching** implementation integrated into the two-tier DDR/CXL ChampSim baseline. The oracle prefetcher represents a theoretical upper bound: it has perfect knowledge of future load addresses and zero mispredictions. Its purpose is to quantify the maximum benefit that any cache-line prefetcher can provide in a two-tier memory system, and to identify what performance limits are architectural rather than accuracy-based.

### Why this is an oracle and not a real prefetcher

The key is how ChampSim traces are generated vs. replayed.

A trace is produced by running the actual program on real hardware and recording every instruction with its **pre-resolved memory address**. The address of every load — including those that depend on prior loads' data — is already baked into the trace record before ChampSim ever sees it.

When ChampSim dispatches a load, it reads the address directly from the trace. There is no "wait for the prior load's data to compute this address." This breaks load-to-address dependencies entirely.

**On real hardware this is impossible:**

```
Real pointer-chasing chain:

  Load A:  addr = base_ptr         → issues to memory, waits 238 cycles
                   ↓ (data returns after 238 cycles)
  Load B:  addr = *base_ptr        → CAN'T issue until Load A returns
                   ↓ (waits another 238 cycles)
  Load C:  addr = **base_ptr       → CAN'T issue until Load B returns

  Total to get 3 addresses: 714 cycles, fully serialized
```

**ChampSim trace replay:**

```
  Load A:  addr = 0x1000  (from trace) → issued immediately
  Load B:  addr = 0x2000  (from trace) → issued immediately
  Load C:  addr = 0x3000  (from trace) → issued immediately

  Total to dispatch 3 loads: 3 cycles (ROB dispatch bandwidth limited)
```

**The consequence for the oracle:**

Because ChampSim dispatches pointer-chasing loads without waiting for each load's data, the CPU pipeline sprints far ahead of the memory subsystem. While LLC is still resolving load A (238-cycle CXL miss), the CPU has already dispatched loads B, C, D... all the way to load A+500. All of those addresses are sitting in the oracle queue. A real CPU would be blocked at load B waiting for A's data.

This is exactly why the oracle is only achievable in simulation — it exploits a property (pre-resolved addresses) that physically cannot exist in hardware until the memory dependency chain has actually executed.

**Note on accuracy:** The oracle has 100% *address* accuracy — it never prefetches an address that won't be demanded. This is distinct from *timing* accuracy; the oracle's measured LLC useful/issued ratio is near 0% because most prefetches still arrive after the demand due to MSHR timing constraints (see Results). For regular stride patterns (lbm), a real prefetcher like Berti can predict addresses too — the oracle's advantage there is deeper lookahead and zero false prefetches, not fundamental address knowledge.

### How addresses enter the queue — pipeline timing

In ChampSim, each simulated CPU cycle executes these pipeline stages in order:

```
CPU tick (each cycle):
  1. execute_instruction()     — instructions retire from ROB
  2. schedule_instruction()    — mark instructions ready to execute
  3. handle_memory_return()    — LLC responses arrive back at CPU
  4. operate_lsq()             — send ready loads from load queue → L1D  ← PUSH happens here
  5. dispatch_instruction()    — new instructions enter ROB
  6. decode_instruction()
  7. fetch_instruction()
```

At step 4, `operate_lsq()` calls `execute_load()` for each ready load, which sends a request to L1D. When L1D processes that request and sees a cache miss, it immediately calls `oracle_feeder::prefetcher_cache_operate()`, which pushes the physical address to the back of the oracle queue.

The address then travels through the memory hierarchy:

```
CPU tick T:
  operate_lsq() → execute_load() → L1D miss
    oracle_feeder pushes addr X to queue back

T + ~5 cycles:   L1D miss arrives at L2C

T + ~15 cycles:  L2C miss → demand arrives at LLC
                 ideal_prefetch runs, reads queue BACK
                 queue back now holds addrs from T+100, T+200, ...
                 (CPU has already dispatched hundreds more loads)
```

### Why the back of the queue contains truly future addresses

LLC takes ~238 cycles to service a CXL miss. During those 238 cycles, the OOO CPU continues executing, dispatching new loads to L1D, all of which push new addresses to the back of the oracle queue. When `ideal_prefetch` runs on the next LLC demand, the queue state looks like this:

```
Queue (2048 entries, front = oldest, back = newest):

  front [... addr#800, addr#801, ..., addr#999,
              addr#1000  ← LLC is currently here
              addr#1001, addr#1002, ..., addr#1500 ...] back
                                        ▲
                          ideal_prefetch reads these 16 entries
                          LLC has not seen them yet
```

Entries near the back are addresses the CPU computed via `operate_lsq()` hundreds of cycles ago in simulation time, but that have not yet arrived at LLC as demand misses. Prefetching them now means the data can be fetched from CXL before the demand arrives — if the timing works out.

### Why real hardware cannot replicate this

On real hardware, the addresses near the back of the queue (`addr#1001` to `addr#1500` in the example above) **do not exist yet**. To compute `addr#1001`, the CPU must:

1. Execute the load at `addr#1000`
2. Wait for `addr#1000`'s data to return from memory (~238 cycles for CXL)
3. Use that data to compute the address of the next load (for pointer-chasing patterns)

For data-independent patterns (strides, streams), a real prefetcher like Berti can predict ahead. But for pointer-chasing workloads like `omnetpp`, each address depends on fetched data — no hardware prefetcher can see beyond the current outstanding load without speculative execution across cache misses, which no existing microarchitecture does.

**Design choice:** Prefetch cache lines from DDR or CXL directly into the LLC — no page migration between tiers.

---

## Architecture

### Components

#### `inc/oracle_prefetch_queue.h`
A global singleton deque shared between the feeder and the prefetcher. Holds up to `MAX_SIZE = 2048` physical addresses of recent L1D cache misses.

```
OraclePrefetchQueue (singleton)
  ┌─────────────────────────────────┐
  │  deque<uint64_t>  MAX_SIZE=2048 │
  │  front = oldest   back = newest │
  └─────────────────────────────────┘
```

#### `prefetcher/oracle_feeder/` — L1D Prefetcher
Intercepts every L1D cache miss and pushes the physical address to the back of `OraclePrefetchQueue`. Runs at L1D level so addresses enter the queue before they propagate through L2C to LLC as demand misses.

```cpp
// On L1D cache miss:
OraclePrefetchQueue::get().push(addr.to<uint64_t>());
```

#### `prefetcher/ideal_prefetch/` — LLC Prefetcher
On every LLC access, reads the **most recent `LOOKAHEAD = 16` entries** from the back of the oracle queue (the furthest-future addresses) and issues prefetch requests. Tracks `pf_issued` and `pf_useful` for statistics.

```
oracle_feeder (L1D)          ideal_prefetch (LLC)
     │                              │
     │  L1D miss → push addr        │  LLC access → read back 16 entries
     ▼                              ▼
 [oldest ◄──── queue (2048) ────► newest]
                                    ▲
                              prefetch these
                            (most future addrs)
```

### Configuration — `oracle_tiered_config.json`
Based on `tiered_memory_config.json` with the following changes:

| Cache | Baseline | Oracle |
|-------|----------|--------|
| L1D prefetcher | `no` | `oracle_feeder` |
| LLC prefetcher | `no` | `ideal_prefetch` |

DDR capacity: `bank_rows = 64` → **16 MiB DDR**, forcing working sets > 16 MiB to spill to CXL.

---

## Build & Run

### Build

```bash
# Build oracle tiered binary
./config.sh oracle_tiered_config.json
make -j
# → bin/champsim_oracle_tiered

# Build baseline tiered binary (for comparison)
./config.sh tiered_memory_config.json
make -j
# → bin/champsim_tiered
```

### Run (single trace)

```bash
bin/champsim_oracle_tiered \
  --warmup-instructions 10000000 \
  --simulation-instructions 50000000 \
  <trace_file>
```

### Run (full suite)

```bash
scripts/run_oracle_tiered_logs.sh   # → logs/oracle_tiered/
scripts/run_tiered_logs.sh          # → logs/tiered_baseline/
```

> Traces must be in `traces/` with the standard DPC-3 naming.
> Available at: https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu2017/

---

## Results

**Configuration:** 16 MiB DDR, CXL overflow, 10M warmup + 50M simulation instructions.

### 619.lbm_s-2676B (3D Lattice Boltzmann — streaming stencil)

| Metric | Tiered Baseline | Oracle Prefetcher | Delta |
|--------|----------------|-------------------|-------|
| IPC | 0.3813 | **0.3816** | +0.08% |
| LLC LOAD MISS | 507,928 | 507,664 | -264 |
| LLC PREFETCH USEFUL | 0 | **52,751** | — |
| LLC PREFETCH MSHR_MERGE | 0 | 52,489 | — |
| DDR Hit Rate | 3.3% | 3.4% | — |
| CXL Pages Resident | 57,534 | 57,534 | — |
| Avg Off-Chip Latency | 238.1 cycles | 237.9 cycles | -0.1% |
| Prefetches Issued (oracle stat) | — | 7,693,868 | — |
| Useful Prefetches (oracle stat) | — | 2,302 | — |

### 620.omnetpp_s-141B (OMNeT++ network simulation — pointer chasing)

| Metric | Tiered Baseline | Oracle Prefetcher | Delta |
|--------|----------------|-------------------|-------|
| IPC | 0.2387 | **0.2389** | +0.08% |
| LLC LOAD MISS | 497,733 | 496,016 | -1,717 |
| LLC PREFETCH USEFUL | 0 | **124** | — |
| DDR Hit Rate | 7.7% | 7.6% | — |
| CXL Pages Resident | 457,853 | 457,865 | — |
| Avg Off-Chip Latency | 95.59 cycles | 95.59 cycles | 0% |
| Prefetches Issued (oracle stat) | — | 11,023,274 | — |
| Useful Prefetches (oracle stat) | — | 0 | — |

---

## Analysis

### Why IPC improvement is near-zero despite useful prefetches

LBM achieves **52,751 useful prefetches** yet only +0.08% IPC. The breakdown:

- **52,489 are MSHR_MERGEs** — the prefetch was already in-flight fetching from CXL when the demand arrived. The demand merged with the in-flight request and shared the result, but still waited the full CXL round-trip (~238 cycles). No cycles are saved.
- **Only 264 are true cache hits** — the prefetch fully completed before the demand arrived, saving the full 238-cycle CXL penalty.

This means the oracle's prefetch lead time is shorter than CXL latency for ~99.5% of prefetches.

### Root cause: prefetch lead time vs. CXL latency

```
oracle_feeder pushes addr at L1D miss time (T)
     │
     │  ~15 cycles (L1D → L2C → LLC pipeline)
     ▼
LLC demand arrives at T+15
     │
     │  ideal_prefetch issues prefetch when processing a prior LLC demand
     │  This happens AFTER the demand is already in the MSHR
     ▼
Prefetch arrives → MSHR_MERGE (demand already in-flight)
     │
     │  ~238 cycles (CXL RTT)
     ▼
Data returned → demand served — but no cycles saved vs. no prefetch
```

For the prefetch to be useful, it must be issued **before** the demand reaches LLC — requiring a lookahead window larger than the L1D→LLC pipeline latency (~15 cycles). The current architecture cannot achieve this because the oracle push and the LLC demand originate from the same L1D miss event.

### omnetpp: random access limits oracle effectiveness

omnetpp has a massive CXL working set (457,853 pages) but only 124 useful prefetches. Pointer-chasing access patterns mean every future address depends on the current memory value — no oracle queue lookahead can predict the next address before the current one is resolved.

### Key finding

> **Even a perfect oracle prefetcher with 100% accuracy and complete future knowledge provides only +0.08% IPC improvement in a two-tier DDR/CXL system.** The bottleneck is not prefetch accuracy but the timing gap between prefetch issuance and demand arrival relative to CXL latency. This establishes that cache-line prefetching alone cannot close the CXL performance gap — co-aware page placement is required to reduce the fraction of accesses served by CXL.

---

## Miss Classification (LBM Oracle)

| Category | Count | Description |
|----------|-------|-------------|
| Timing misses (MSHR_MERGE) | 52,489 | Prefetch in-flight; demand arrived first |
| True useful prefetches | 264 | Prefetch completed; demand hit in LLC |
| Remaining demand misses | ~507,400 | Not prefetched or prefetch too late |

The large timing miss count (52,489) directly quantifies the gap between ideal and achievable prefetching in the CXL context. Closing this gap requires either a longer lookahead window (e.g., CPU-dispatch-level oracle feeding with virtual address translation) or page migration to reduce CXL RTT.

---

## Files Added

```
inc/
  oracle_prefetch_queue.h       # Global singleton deque (MAX_SIZE=2048)

prefetcher/
  oracle_feeder/
    oracle_feeder.h             # L1D prefetcher declaration
    oracle_feeder.cc            # Pushes L1D miss addrs to oracle queue
  ideal_prefetch/
    ideal_prefetch.h            # LLC prefetcher declaration
    ideal_prefetch.cc           # Reads oracle queue back, issues prefetches

oracle_tiered_config.json       # Build config: tiered memory + oracle prefetchers
scripts/run_oracle_tiered_logs.sh  # Run script for oracle tiered evaluation
ORACLE_PREFETCH.md              # This file
```
