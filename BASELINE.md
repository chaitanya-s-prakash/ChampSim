# Project Baseline Guide

This document explains the current project baseline built on top of
ChampSim. It explains what has already been implemented, how to build
the binaries, how to reproduce the baseline runs, and how to
sanity-check the outputs on their own machine.

## Purpose

The current baseline is the shared starting point for the project. It
is not the final co-aware placement + prefetching design. It provides:

- a standard single-DDR baseline
- a naive two-tier DDR + CXL baseline
- a common workload set
- a common metric set
- a common run and logging workflow

This gives everyone on the team the same reference point before moving
on to oracle baselines, M5, Berti, and the final co-aware design.

## What Is Implemented

The current branch covers the baseline infrastructure only.

### 1. Simulator and framework

- ChampSim is the simulation framework used for this project.
- The repository contains the baseline single-memory configuration and
  the modified two-tier DDR + CXL configuration.

Relevant files:

- `champsim_config.json`
- `tiered_memory_config.json`

### 2. Single-DDR baseline

- A standard one-core, one-DDR configuration is available through
  `champsim_config.json`.
- This is used as the main reference baseline.

### 3. Two-tier DDR + CXL baseline

- A naive two-tier memory system has been implemented.
- Pages are allocated into DDR until DDR capacity is exhausted.
- Additional pages spill into the slower CXL tier.
- Accesses to the slow tier experience higher latency than DDR.

Important note:

This is a simplified tiered-memory model. It is not a full CXL protocol
model. At this stage, the code models tier capacity, placement by
spillover, and higher latency for the far tier.

### 4. Workloads

The standardized baseline workload set is:

- `600.perlbench_s-210B.champsimtrace.xz`
- `605.mcf_s-1152B.champsimtrace.xz`
- `619.lbm_s-2676B.champsimtrace.xz`
- `620.omnetpp_s-141B.champsimtrace.xz`

All traces are expected to be placed in `traces/`.

### 5. Metrics currently reported

The baseline currently reports the following metrics:

- DDR tier access rate
- hierarchy AMAT estimate
- execution time through ROI cycles and IPC
- per-tier bandwidth utilization
- DDR/CXL page residency
- DRAM/CXL queue occupancy

Metric notes:

- `Execution time` should be interpreted as ROI cycles or IPC, not host
  wall-clock runtime.
- `HIERARCHY_AMAT_ESTIMATE` is a derived hierarchy-level AMAT estimate
  using the weighted average latency across all memory tiers. Separate
  `AVERAGE DDR DEMAND ACCESS LATENCY` and `AVERAGE CXL DEMAND ACCESS
  LATENCY` lines are printed when both tiers are active.
- `DDR TIER ACCESS RATE` is the fraction of total off-chip demand reads
  served from DDR (not a DRAM row-buffer hit rate; those are reported
  separately as `RQ ROW_BUFFER_HIT`).
- `DEMAND_TIER_ACCESSES` is printed inside each DRAM/CXL channel block.
  At the current baseline stage it is effectively equal to that
  channel's `DEMAND_REQUESTS`. The useful signal is the DDR vs CXL split
  captured by the top-level `DDR TIER ACCESS RATE`.
- `Memory occupancy` in the project writeup most naturally refers to
  DDR/CXL page residency.
- Queue occupancy is also exported as an additional controller-level
  pressure metric.
- `CURRENT_DDR_PAGES` and `CURRENT_CXL_PAGES` count **all** physical
  pages allocated in each tier, including page table entry (PTE) pages
  created during address translation.
- In a 5-level page table with 4KB pages, PTE pages can account for
  several thousand entries on top of data page allocations.

## What Is Not Implemented Yet

The following are not part of the baseline yet:

- dynamic page migration policies
- oracle placement
- oracle prefetching
- M5 placement
- Berti prefetching
- tier-aware prefetch adaptation
- prefetchability-aware placement
- the final co-aware M5 + Berti design

So the repository is currently baseline-ready, not research-complete.

## Important Files

Configuration files:

- `champsim_config.json`
- `tiered_memory_config.json`

Run scripts:

- `scripts/run_trace_suite.sh`
- `scripts/run_ddr_logs.sh`
- `scripts/run_tiered_logs.sh`

Shared metric helper:

- `inc/stats_utils.h`

Output directories:

- `logs/ddr_baseline/`
- `logs/tiered_baseline/`

## Build Workflow

Build selection is manual. Log collection is automated.

Do not run the two build flows in parallel in the same repository. Both
flows write shared generated files during configuration and build.

Build the single-DDR binary:

```bash
./config.sh champsim_config.json
make -j
```

This produces:

```text
bin/champsim
```

Build the tiered DDR + CXL binary:

```bash
./config.sh tiered_memory_config.json
make -j
```

This produces:

```text
bin/champsim_tiered
```

Because the two configuration files use different executable names, both
binaries can coexist in `bin/`.

## Run Workflow

Once the correct binaries have been built, log collection is automated
through the scripts.

Run the DDR baseline suite:

```bash
scripts/run_ddr_logs.sh
```

Run the tiered baseline suite:

```bash
scripts/run_tiered_logs.sh
```

Default run lengths:

- warmup instructions: `10000000`
- simulation instructions: `50000000`

The scripts expect the four standardized traces to already exist under
`traces/`.

### Custom instruction counts

You can override the defaults:

```bash
scripts/run_ddr_logs.sh 1000000 5000000
scripts/run_tiered_logs.sh 1000000 5000000
```

## Log Layout

The scripts create logs in separate folders for each configuration:

```text
logs/ddr_baseline/
logs/tiered_baseline/
```

Each folder contains:

- one log per trace
- `_run_info.txt`

Example files:

```text
logs/ddr_baseline/mcf_s-1152B.log
logs/ddr_baseline/perlbench_s-210B.log
logs/tiered_baseline/mcf_s-1152B.log
logs/tiered_baseline/perlbench_s-210B.log
```

The `_run_info.txt` file records:

- the label used by the run script
- the binary path
- warmup instruction count
- simulation instruction count
- trace directory
- timestamp

## How To Sanity-Check A Run

After a successful run, each log should contain:

- `Warmup complete`
- `Simulation complete`
- CPU statistics
- cache statistics
- DRAM statistics
- virtual memory statistics

For the metric portion, look for:

- `DDR TIER ACCESS RATE`
- `AVERAGE OFF-CHIP DEMAND ACCESS LATENCY`
- `AVERAGE DDR DEMAND ACCESS LATENCY` (tiered runs only)
- `AVERAGE CXL DEMAND ACCESS LATENCY` (tiered runs only)
- `DEMAND_TIER_ACCESSES` inside each DRAM/CXL channel block
- `HIERARCHY AMAT ESTIMATE`
- `BANDWIDTH UTILIZATION`
- `AVERAGE TOTAL QUEUE OCCUPANCY`
- `CURRENT_DDR_PAGES`
- `CURRENT_CXL_PAGES`

## Expected Baseline Behavior

The current DDR and tiered baselines should behave roughly as follows.

### DDR baseline

- all pages reside in DDR
- `CURRENT_CXL_PAGES` should stay `0`
- `DDR TIER ACCESS RATE` should be `1` for these runs

### Tiered baseline

- DDR fills first, then extra pages spill into CXL
- memory-intensive traces should show non-zero `CURRENT_CXL_PAGES`
- `DDR TIER ACCESS RATE` should drop for traces that spill heavily
- `perlbench` may remain largely unchanged if it fits in DDR

This is exactly the kind of separation the baseline is intended to
create.

## Recommended Validation Steps

1. Build `bin/champsim` from `champsim_config.json`.
2. Build `bin/champsim_tiered` from `tiered_memory_config.json`.
3. Confirm that all four traces exist under `traces/`.
4. Run `scripts/run_ddr_logs.sh`.
5. Run `scripts/run_tiered_logs.sh`.
6. Confirm that logs appear in both `logs/ddr_baseline/` and
   `logs/tiered_baseline/`.
7. Open one DDR log and one tiered log and verify the expected metrics
   are present.

The Two Gaps
Gap 1 — Prefetcher cannot reach MEMORY_CONTROLLER

A prefetcher is bound to its parent CACHE* via bound_to<CACHE> in inc/modules.h:102. The physical address IS available in prefetcher_cache_operate(), but is_cxl_address() lives on MEMORY_CONTROLLER, which is only accessible through env.dram_view() in src/champsim.cc:177 — not through CACHE.

So Berti cannot call is_cxl_address(addr) as written. The teammate implementing Berti has two clean options:

Option A (recommended): Add a bool is_cxl_address(champsim::address) const method to CACHE that forwards to the downstream memory controller. This is a small addition to cache.h/cache.cc.
Option B: Encode tier in pf_metadata (a uint32_t already threaded through the prefetcher pipeline) — the memory controller sets a bit when serving a CXL request, and Berti reads it on the next fill.
Gap 2 — VirtualMemory has no migration API

M5 needs to move pages between DDR and CXL at runtime. Currently VirtualMemory handles address translation one way — once a virtual page is mapped to a physical page, that mapping never changes. The page_residency map, vpage_to_ppage_map, and record_allocation() are all private.

The teammate implementing M5 will need to add a public migrate_page(cpu, vpage, target_tier) method to VirtualMemory that:

Finds a free physical page in the target tier
Updates vpage_to_ppage_map to point to the new physical address
Updates page_residency
The data structures are already right for this — ppage_free_list spans both tiers and page_tier() can classify any page.