# M5 Page Migration Policy — ChampSim Implementation

## Overview

This document describes the implementation of the M5 tiered-memory page migration
policy inside ChampSim. M5 is a hardware-assisted page migration mechanism designed
for systems with a fast DDR tier and a slower CXL-attached memory tier. The key idea
is to track hot pages near the CXL controller (not in software on the CPU), use
aggregate bandwidth density as a migration trigger, and rely on an OS-like recency
policy to select DDR victims when DDR is full.

The implementation is a **simulator model** of the M5 paper's policy — not a
reproduction of the paper's Linux kernel or FPGA artifact. It is designed to let
you evaluate the architectural benefit of CXL-side hot-page tracking and
bandwidth-density-driven migration against a static tiered-memory baseline.

---

## System Architecture

The simulated system has two memory tiers:

```
CPU  →  L1I/L1D  →  L2C  →  LLC  →  MEMORY_CONTROLLER
                                          ├── DDR Channel(s)   (fast, small)
                                          └── CXL Channel(s)   (slow, large)
```

- **DDR**: Low-latency DRAM (tCAS=24, data_rate=3200 MT/s in default config)
- **CXL**: Higher-latency DRAM accessed over a CXL link (tCAS=40, data_rate=1600 MT/s)
- **Initial placement**: Fill DDR first, then spill to CXL (fill-DDR-first policy)
- **Migration**: M5Manager periodically promotes hot CXL pages to DDR and demotes
  cold DDR pages to CXL to make room

---

## What Was Built

### Step 1 — Configuration Parameters (`inc/m5_policy.h`)

All M5 knobs are centralized in `m5::config`. Key parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `page_size` | 4096 B | Page granularity |
| `cacheline_size` | 64 B | Cache-line granularity |
| `migration_bandwidth_GBps` | 10.0 | Page-copy bandwidth between tiers |
| `migration_epoch_instrs` | 10M | Instructions between migration decisions |
| `hpt_cms_width/depth` | 8192/4 | Count-Min Sketch dimensions for HPT |
| `hpt_top_k` | 5 | Hot-page candidates reported per epoch |
| `hwt_cms_width/depth` | 8192/4 | Count-Min Sketch dimensions for HWT |
| `hwt_top_k` | 40 | Hot cache-line candidates (= hpt_top_k × hwt_density_threshold) |
| `hwt_density_threshold` | 8 | Min hot lines per page to qualify for migration |
| `bw_density_ratio_threshold` | 1.0 | CXL/DDR density ratio to trigger migration |
| `max_migrations_per_epoch` | 5 | Max promotions per epoch |
| `mglru_generations` | 4 | MGLRU generation count (0=hot, 3=cold victim) |
| `migration_cooldown_epochs` | 2 | Epochs a page is frozen after migration |

These defaults are also mirrored in `tiered_memory_config.json` under `"m5_policy"`.

---

### Step 2 — Explicit Page Placement (`inc/page_placement.h`, `src/page_placement.cc`)

**Problem**: The original ChampSim tiered memory permanently determined a page's
tier from its physical address range. Migration requires changing a page's tier
without changing its CPU-visible address.

**Solution**: A `PagePlacementTable` that separates two concepts:

- **Logical physical page**: The stable address the CPU and caches use (never changes)
- **Routed physical page**: The actual DRAM slot the request is sent to (swapped on migration)

```
Before migration:  Page A (CXL addr) → routes to CXL slot A
                   Page B (DDR addr) → routes to DDR slot B

After migration:   Page A (CXL addr) → now routes to DDR slot B  ← promoted
                   Page B (DDR addr) → now routes to CXL slot A  ← demoted
```

The CPU still uses address A and address B. Only the DRAM routing changes.

**Lazy registration**: Pages are registered on first access. Initial tier is
determined from the address range (same as original ChampSim), so baseline
behavior is unchanged until `migrate()` is first called.

**MGLRU metadata**: Each entry also tracks a `generation` (0=hot, max=cold) for
victim selection, and a `cooldown_epochs` counter for anti-ping-pong protection.

**Key methods**:
- `get_routed_address(addr)` — translates logical → routed address (called in every memory request)
- `migrate(cxl_ppage, ddr_ppage)` — swaps routing between a CXL candidate and DDR victim
- `touch(ppage)` — resets generation to 0 when a DDR page is accessed
- `age_all_ddr_pages(max_gen)` — ages all DDR pages one generation colder per epoch
- `get_ddr_victim(max_gen)` — returns the coldest DDR page with no cooldown
- `set_cooldown(ppage, epochs)` — freezes a page after migration

**Integration**: `MEMORY_CONTROLLER::add_rq()` and `add_wq()` call
`get_routed_address()` on every memory request before channel selection.

---

### Step 3 — Latency Fix (`src/dram_controller.cc`)

A pre-existing bug was found and fixed: when the CXL channel has a different
clock period than the primary DDR memory controller, `time_enqueued` was being
set using the DDR MC's clock while latency was measured in CXL clock units.
This caused CXL latency to grow proportionally with simulation length
(reported as ~1.06×10^7 cycles instead of the expected ~73–600 cycles).

**Fix**: `time_enqueued` now uses `channel.current_time` (the channel's own
clock domain) instead of `MEMORY_CONTROLLER::current_time`.

---

### Step 4 — M5 Monitor (`inc/m5_monitor.h`, `src/m5_monitor.cc`)

The `M5Monitor` tracks per-epoch bandwidth density — the core signal that tells
the Elector whether CXL is "hotter per page" than DDR.

**Bandwidth density** = bytes served by a tier in an epoch / pages resident in that tier.

If `cxl_density / ddr_density >= bw_density_ratio_threshold`, CXL contains hotter
pages per unit capacity than DDR — migration is likely beneficial.

The monitor uses a snapshot/delta pattern: at each epoch boundary it computes the
difference between current cumulative channel bytes and the previous epoch's snapshot.

**Output** (printed at end of simulation):
```
M5 Monitor Bandwidth Density (lifetime ROI)
  DDR READ BYTES:          6591296  CXL READ BYTES:        28873472
  DDR PAGES:                 16128  CXL PAGES:               33536
  DDR BW DENSITY:          408.72 bytes/page
  CXL BW DENSITY:          860.99 bytes/page
  CXL/DDR DENSITY RATIO:    2.1067  (>1 means migration is beneficial)
```

---

### Step 5 — HPT and HWT Trackers (`inc/cms_tracker.h`, `src/cms_tracker.cc`)

Two CXL-side trackers observe every demand read that reaches a CXL channel.
They are **never** updated by DDR accesses or cache hits.

#### Count-Min Sketch (CMS)

A `depth × width` counter matrix. Each row has an independent hash function.

- **Update key K**: for each row `r`, increment `table[r][hash_r(K) % width]`
- **Estimate count of K**: take the minimum across all rows

Collisions can only inflate counts (never reduce them), so the minimum is the
tightest estimate. With `width=8192` and `depth=4`, error is bounded at ~1.8%
probability of exceeding 0.012% of total accesses.

#### Top-K List

A bounded list of `(key, estimated_count)` pairs. On every update, if the new
estimate beats the current minimum, the minimum is evicted and the new entry
takes its place. This gives an online approximation of the K hottest keys.

#### HPT (Hot Page Tracker)
- Key: logical page number
- Updated per CXL demand read
- Reports the `hpt_top_k` (5) hottest CXL pages per epoch

#### HWT (Hot Word/Cache-Line Tracker)
- Key: `page_num × lines_per_page + line_offset_in_page`
- Updated per CXL demand read (same accesses as HPT)
- Reports the `hwt_top_k` (40) hottest cache lines per epoch
- Used by the Nominator to count how many distinct hot lines are in each HPT candidate page

**Why `hwt_top_k = hpt_top_k × hwt_density_threshold = 5 × 8 = 40`**:
To give each of the 5 HPT candidates a fair chance of satisfying the density
threshold of 8 hot lines, the HWT must be able to track at least 40 distinct
hot cache lines simultaneously.

**Integration**: Both trackers are reset at every epoch boundary by M5Manager.

---

### Step 6 — MGLRU-like Victim Selection (`inc/page_placement.h`)

When a CXL page is promoted to DDR, DDR is usually full. The policy needs to
evict the coldest DDR page. MGLRU approximates LRU recency with a small number
of generations instead of an exact ordering.

```
Generation 0  →  recently accessed (hottest)
Generation 1  →  warm
Generation 2  →  cool
Generation 3  →  not accessed in a long time (victim candidate)
```

**Touch**: Every DDR-bound demand read calls `placement_table.touch(ppage)`,
resetting that page's generation to 0.

**Aging**: At each epoch, `age_all_ddr_pages()` increments every DDR page's
generation by 1 (capped at `mglru_generations - 1`). Pages that weren't
touched during the epoch get colder.

**Victim selection**: `get_ddr_victim()` scans from the coldest generation
downward and returns the first DDR page with no active cooldown. This is an
OS-like recency approximation, not an oracle — it does not use global hotness
knowledge.

---

### Step 7 — Migration Cost Model (`inc/m5_migration.h`)

Migration is not free. Copying a 4 KB page at 10 GB/s takes:

```
cost = 4096 bytes / (10 × 10^9 B/s) = 409.6 ns = 1638 CPU cycles at 4 GHz
```

`compute_migration_cost_cycles(page_size, bandwidth_GBps, cpu_freq_MHz)` computes
this at runtime. M5Manager accumulates `total_migration_cost_cycles` for each
promotion to report the total overhead of migration.

**Timing model**: Placement updates take effect immediately after `migrate()` is
called (first-version simplification). The cost is tracked as a stat and reflects
the real bandwidth consumed — at 5 migrations per epoch, only 8190 cycles of
overhead are incurred versus a 10M-instruction epoch.

`MigrationStats` collects all migration accounting:
- Promotions, demotions, total cost
- Pages skipped (density filter, bandwidth-density check, cooldown, no victim)
- Epochs fired vs skipped

---

### Step 8 — M5Manager: The Full Pipeline (`inc/m5_manager.h`, `src/m5_manager.cc`)

M5Manager orchestrates all four policy roles in a single epoch-driven pipeline
triggered every `migration_epoch_instrs` retired CPU instructions.

#### Trigger mechanism

`O3_CPU::operate()` calls `m5::g_m5_manager->maybe_trigger_epoch(num_retired)`
after every instruction retirement check. When enough instructions have elapsed,
the full pipeline fires.

`M5Manager` is initialized in `champsim::main()` with a reference to
`MEMORY_CONTROLLER` and the global pointer `g_m5_manager` is set before any
simulation phase begins.

#### Pipeline

```
maybe_trigger_epoch(num_retired)  [fires every 10M instructions]
│
├─ 1. MONITOR
│     Sum bytes_returned across DDR and CXL channels.
│     Call monitor.reset_epoch() → get EpochStats with density_ratio.
│
├─ 2. NOMINATOR
│     For each page in hpt.top_entries():
│       • Skip if not CXL-resident (already promoted)
│       • Skip if in cooldown  [→ skipped_cooldown]
│       • Count HWT entries belonging to this page (hot line density)
│       • Skip if hot_lines < hwt_density_threshold  [→ skipped_density_filter]
│       → Survivors are promotion candidates
│
├─ 3. ELECTOR
│     • Need ≥ min_candidates_per_epoch candidates
│     • Need density_ratio ≥ bw_density_ratio_threshold  [→ skipped_bw_density_trigger]
│     • If either check fails → skip epoch, go to Reset
│
├─ 4. PROMOTER  [up to max_migrations_per_epoch times]
│     For each candidate:
│       • get_ddr_victim(max_gen) → coldest DDR page with no cooldown
│       • If no victim found → stop  [→ skipped_no_victim]
│       • placement_table.migrate(cxl_ppage, ddr_victim)
│       • set_cooldown() on both pages for migration_cooldown_epochs epochs
│       • Accumulate migration cost in stats
│
└─ 5. RESET AND AGE
      • hpt.reset() + hwt.reset()  (clear CMS counters and top-K list)
      • age_all_ddr_pages()  (every mglru_aging_interval_epochs epochs)
        - Increments generation of all DDR pages not recently touched
        - Ticks down cooldown counters for all pages
```

#### What makes this paper-faithful

- HPT/HWT observe **only CXL-bound demand reads** — never DDR accesses, never cache hits
- Monitor uses **aggregate** DDR/CXL bandwidth density, not per-page DDR hotness
- DDR victims are selected by **MGLRU-like recency**, not oracle global hotness
- Migration has **nonzero cost** based on configured copy bandwidth
- CXL candidates come from **CXL-side HPT/HWT**, not a global tracker

---

### Step 9 — Stats and Output (`src/plain_printer.cc`, `inc/phase_info.h`)

Migration stats use a **snapshot/delta pattern** matching DRAM's `roi_stats`:
- `begin_roi()` snapshots cumulative stats at the warmup→ROI boundary
- `roi_migration_stats()` returns the delta (ROI-only, warmup excluded)

Stats are stored in `phase_stats.m5_migration_stats` and printed at simulation end:

```
M5 Migration Statistics (ROI)
  EPOCHS FIRED:                       10
  EPOCHS WITH MIGRATION:               3
  EPOCHS SKIPPED (elector):            7

  TOTAL PROMOTIONS (CXL→DDR):         12
  TOTAL DEMOTIONS  (DDR→CXL):         12
  TOTAL MIGRATION COST:            19656 cycles

  SKIPPED density filter:              8
  SKIPPED bw-density trigger:          5
  SKIPPED cooldown:                    2
  SKIPPED no DDR victim:               0
```

---

## File Structure

```
inc/
  m5_policy.h          — All M5 configuration knobs (m5::config)
  page_placement.h     — Explicit page placement table with MGLRU metadata
  cms_tracker.h        — Count-Min Sketch + top-K tracker (used for HPT and HWT)
  m5_monitor.h         — Per-epoch bandwidth density tracker (Monitor)
  m5_migration.h       — Migration cost function + MigrationStats struct
  m5_manager.h         — M5Manager: the full epoch-driven pipeline

src/
  page_placement.cc    — PagePlacementTable implementation
  cms_tracker.cc       — CmsTracker implementation
  m5_monitor.cc        — M5Monitor implementation
  m5_manager.cc        — M5Manager implementation

Modified files:
  inc/dram_controller.h     — Added placement_table, monitor, hpt, hwt members
  src/dram_controller.cc    — Routing via placement table; HPT/HWT/MGLRU updates;
                               latency fix (time_enqueued uses channel clock)
  src/ooo_cpu.cc            — Epoch trigger call after heartbeat
  src/champsim.cc           — M5Manager initialization; begin_roi() at phase boundary
  inc/phase_info.h          — Added m5_migration_stats to phase_stats
  inc/stats_printer.h       — Added format(m5::MigrationStats) declaration
  src/plain_printer.cc      — M5 stats formatter; bandwidth density output
  tiered_memory_config.json — Added m5_policy configuration block
```

---

## How to Build and Run

```bash
# Configure and build
./config.sh tiered_memory_config.json
make -j$(nproc)

# Run simulation
bin/champsim_tiered \
    --warmup-instructions 50000000 \
    --simulation-instructions 100000000 \
    <path-to-trace>
```

---

## Interpreting the Output

At the end of simulation, look for three sections:

### 1. M5 Monitor Bandwidth Density
Validates that the Monitor is computing meaningful density signals.
`CXL/DDR DENSITY RATIO > 1` confirms that CXL contains hotter pages per unit
capacity than DDR — the condition under which M5 migration should help.

### 2. M5 Migration Statistics
Shows what the policy actually did during the ROI phase.

**If `EPOCHS FIRED = 0`**: The trigger is not wiring up — check that `g_m5_manager`
is being set in `champsim::main()`.

**If `EPOCHS WITH MIGRATION = 0`**: The Elector is always saying no. Check:
- `SKIPPED density filter` — if high, reduce `hwt_density_threshold` or set to 0
- `SKIPPED bw-density trigger` — if high, the workload's hot set fits in DDR already
- Whether `hpt_top_k` and `hwt_top_k` are sized correctly

**If `SKIPPED no DDR victim` is high**: All DDR pages are in cooldown. Reduce
`migration_cooldown_epochs`.

### 3. DRAM Statistics
Compare DDR vs CXL access counts and latencies. With effective migration:
- `DDR TIER ACCESS RATE` should be higher than in the baseline (more traffic going to fast DDR)
- `CXL AVERAGE DEMAND ACCESS LATENCY` may decrease (less CXL traffic = less queuing)

---

## Comparison Methodology

The intended baseline for comparison is the **original ChampSim tiered memory**
(rigid address-range routing, no explicit placement table, no migration).

To make the comparison fair:
1. Apply the Step 3 latency fix (`time_enqueued = channel.current_time`) to the
   original codebase as well — the original code had inflated CXL latency due to
   a clock-domain mismatch.
2. Run both binaries on the same trace with the same warmup/simulation instruction
   counts and the same DDR/CXL memory configuration.
3. Compare: IPC, LLC miss latency, DDR access rate, AMAT estimate.

**Primary metrics for M5 benefit**:
```
IPC improvement      = (IPC_m5 - IPC_base) / IPC_base × 100%
AMAT reduction       = (AMAT_base - AMAT_m5) / AMAT_base × 100%
DDR access rate gain = DDR_rate_m5 - DDR_rate_base  (percentage points)
Migration overhead   = total_migration_cost_cycles / total_sim_cycles × 100%
```

---

## Known Limitations and Simplifications

1. **Migration timing**: Placement updates take effect immediately after `migrate()`
   is called. The paper models in-flight migration with per-access stalling during
   the copy window. This is left as future work; the cost is still correctly
   accounted for in `total_migration_cost_cycles`.

2. **Single-copy bandwidth**: Migration traffic is modeled as a time cost
   (`compute_migration_cost_cycles`) but does not actually consume DDR/CXL
   bandwidth in the timing model. A more detailed model would contend migration
   traffic with demand traffic.

3. **CPU frequency hardcoded**: `compute_migration_cost_cycles()` uses 4000 MHz.
   This should match the `frequency` field in the `ooo_cpu` section of the config.

4. **Single-epoch CMS**: HPT/HWT counters are reset at every epoch (epoch-scoped
   tracking). The paper discusses both epoch-scoped and cumulative-with-decay modes.

5. **MGLRU approximation**: The victim selector scans the full placement table
   linearly. For large working sets this is O(N) per epoch but acceptable for
   simulator use.

6. **M5Manager only runs on CPU 0**: The trigger in `ooo_cpu.cc` fires for every
   CPU, but since `g_m5_manager` is a single instance, only the first CPU to
   reach the epoch boundary triggers the pipeline. For multi-core this should be
   handled more carefully.
