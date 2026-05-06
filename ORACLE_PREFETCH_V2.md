# Oracle Prefetcher V2

## Problem with V1: Timing

The original oracle prefetcher (`Oracle` branch) reads the **back of the oracle queue** — the most recently pushed addresses — and issues them as prefetches when each LLC demand arrives.

### Why this fails

`oracle_feeder` runs as the L2C prefetcher and pushes an address to the queue at **L2C miss time**. The same address immediately travels down the L2C→LLC pipeline and arrives at LLC as a demand in **~15 CPU cycles**.

`ideal_prefetch` reads those same back-of-queue entries at LLC demand time and issues a prefetch. But the prefetch and the demand are for the **same address at the same time** — the prefetch can only complete 244 cycles *after* the demand has already arrived.

```
T+0:    L2C miss → oracle_feeder pushes addr to queue
T+15:   addr arrives at LLC as a demand
         ideal_prefetch sees demand, reads back of queue, issues prefetch for same addr
T+15+244 = T+259:  prefetch completes (CXL round trip)
         → demand already merged at T+15, waited full 244 cycles anyway
```

The result is that **~99% of prefetches become MSHR_MERGEs**: the demand merged with the in-flight prefetch request and waited the full CXL RTT regardless. No cycles are saved.

### Measured results (619.lbm_s-2676B, 50M instructions)

| Metric | Value |
|--------|-------|
| IPC (baseline) | 0.3813 |
| IPC (oracle V1) | 0.3816 (+0.08%) |
| LLC PREFETCH USEFUL | 52,751 |
| LLC LOAD MSHR_MERGE | 52,489 (99.5% of useful) |
| True cache hits (pf_useful) | 2,302 |
| Prefetches issued | 7,693,868 |
| Effective accuracy | 0.03% |

Only **2,302 out of 7.7 million prefetches** became true cache hits. 52,489 were MSHR_MERGEs. The remaining ~7.6M were either useless (line evicted before demand) or redundant.

### Validation: near-zero CXL latency

To confirm timing was the cause (not a logic bug), a second run was performed with CXL DRAM parameters reduced to minimum (`tCAS=tRCD=tRP=tRAS=1`):

| | Normal CXL (~244 cycles) | Fast CXL (~50 cycles) |
|---|---|---|
| IPC | 0.3816 | **1.264** (+231%) |
| LLC PREFETCH USEFUL | 52,751 | 31,175 |
| True cache hits | 2,302 | 2,463 |

IPC improved 231% from lower latency alone — not from the prefetcher. MSHR_MERGEs persisted even at 50 cycles because the 15-cycle pipeline transit still beats the prefetch.

---

## CXL Bandwidth — Maximum Possible L1 Hit Rate

A stronger prefetcher would target **L1D** directly, intercepting misses before they propagate. The CXL memory bus sets a hard upper bound on how many cache lines per second can be delivered.

### CXL bandwidth ceiling

From `oracle_tiered_config.json`:

```json
"channel_width": 8,    // bytes (64-bit bus)
"data_rate": 1600,     // MT/s
```

```
Peak CXL bandwidth = 8 B × 1600 MT/s = 12.8 GB/s
Cache line size    = 64 bytes          (block_size in config)
Max line rate      = 12.8 GB/s ÷ 64 B = 200 million lines/second
CPU frequency      = 4000 MHz
→ 1 cache line delivered every 20 CPU cycles (maximum)
```

### lbm demand rate vs. bandwidth limit

From simulation logs (131,025,035 cycles for 50M instructions):

| Level | Load accesses | Load misses | Miss interval |
|-------|-------------|------------|---------------|
| L1D | 3,082,618 | 507,942 (new) | 1 per **258 cycles** |
| L2C | 507,942 | 507,931 | 1 per **258 cycles** |
| LLC | 507,931 | 507,664 | 1 per **258 cycles** |

L1D MSHR_MERGEs account for 666,041 of the 1,173,983 raw L1D load misses — these are loads that issued while a prior miss to the same line was still in-flight. The **507,942 new (non-merged) L1D load misses** each require a unique fetch from memory, one every ~258 cycles.

```
Demand bandwidth required = 64 B / 258 cycles = 0.248 B/cycle = 0.99 GB/s
CXL peak bandwidth        = 12.8 GB/s
Utilization               = 0.99 / 12.8 = 7.7%
```

**CXL bandwidth is not the bottleneck for lbm.** At one demand every 258 cycles and a bandwidth limit of one line every 20 cycles, the bus has headroom for up to **12 simultaneous prefetch streams** before saturation:

```
Max LOOKAHEAD before saturation = floor(258 / 20) = 12
```

V1 uses `LOOKAHEAD = 16` — already slightly over the bandwidth limit per miss interval, causing the last 3–4 prefetches in each batch to queue up and experience additional delay.

### Maximum achievable L1 hit rate

Given sufficient prefetch lead time (≥ 244 cycles before the L1D miss), CXL bandwidth can service every lbm demand without saturation. With a perfect trace-driven prefetcher that knows exactly which address will miss and when:

```
Inter-miss interval:    258 cycles
CXL RTT:                244 cycles
Margin:                  14 cycles
```

A prefetcher issuing one prefetch per demand, timed to complete exactly 14 cycles before the demand arrives, could theoretically achieve **close to 100% L1 true cache hits** for lbm — bandwidth is not a constraint, only timing precision is.

For omnetpp (pointer chasing, irregular access), the same bandwidth ceiling applies but timing is easier: the inter-miss interval is ~420 cycles vs. 96-cycle CXL RTT, giving a 324-cycle margin.

| Workload | CXL RTT | Inter-miss interval | Timing margin | BW saturation LOOKAHEAD |
|----------|---------|-------------------|--------------|------------------------|
| lbm | 244 cycles | 258 cycles | **14 cycles** | 12 |
| omnetpp | 96 cycles | 420 cycles | **324 cycles** | 20 |

lbm's 14-cycle margin is the binding constraint — any prefetcher for lbm must issue its prefetch within a 14-cycle window of the ideal issue time, or the demand will arrive first.

---

## Cache Line Granularity — Another Upper Bound

Each CXL request fetches exactly **one 64-byte naturally-aligned cache line**. This is a hardware invariant: you cannot request non-contiguous bytes in a single transaction, and you cannot request more than 64 bytes per request.

```
prefetch_line(0x1234)  →  fetches bytes [0x1200 .. 0x123F]  (aligned to 64-byte boundary)
```

If a future load accesses an address in a different 64-byte block, it requires a completely separate CXL request, each paying the full latency independently.

### Implication for prefetch coverage

For a workload where loads are spread across many non-contiguous 64-byte blocks (e.g. omnetpp pointer chasing), each unique block requires its own prefetch request. There is no way to "batch" non-contiguous data into a single CXL transfer.

This sets a hard upper bound on **prefetch coverage per unit of bandwidth**:

```
1 CXL request = 64 bytes = 1 cache line = 1 future load covered
To cover N unique cache lines = N separate requests × 20 cycles each = N × 20 cycles of CXL bandwidth
```

For lbm (streaming stencil, mostly sequential addresses):
- Sequential loads often fall in the **same 64-byte block** → multiple loads covered by one prefetch
- Effective prefetch efficiency is high — one request covers several nearby loads

For omnetpp (pointer chasing, random addresses):
- Each load typically lands in a **different 64-byte block** → one prefetch per load, no sharing
- Every future load requires its own request; no batching possible

Combined with the bandwidth limit of 1 line per 20 cycles and lbm's 258-cycle inter-miss interval, the maximum number of **distinct cache lines** that can be prefetched and delivered before the next demand is:

```
lbm:    floor(258 / 20) = 12 unique lines
omnetpp: floor(420 / 20) = 21 unique lines  (within 96-cycle RTT window: floor(324 / 20) = 16)
```

Any prefetcher claiming to cover more than these counts per demand interval will be limited by CXL bus bandwidth, not by MSHR capacity or LLC size.
