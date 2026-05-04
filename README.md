# Berti Prefetcher - ChampSim Implementation

## Overview

This branch implements a MICRO'22 paper-oriented Berti L1D data prefetcher inside
ChampSim and evaluates it in the existing tiered DDR+CXL memory model.

---

## System Architecture

The configured system is:

```text
CPU -> L1I/L1D -> L2C -> LLC -> MEMORY_CONTROLLER
                                  |-> DDR channel  (small, fast tier)
                                  |-> CXL channel  (larger, slower tier)
```

In this branch:

- Berti is attached to the **L1D**.
- L1D prefetches are virtual-address prefetches.
- DDR is filled first by the virtual-memory allocator.
- Pages spill into CXL after the DDR tier reaches capacity.
- Berti does not know whether a target page is in DDR or CXL.

---

## What Was Built

### 1. Berti L1D Prefetcher

Files:

```text
prefetcher/berti/berti.h
prefetcher/berti/berti.cc
```

The implementation includes the core paper structures:

| Structure | Implementation |
|---|---|
| History table | 8 sets, 16 ways, FIFO |
| History IP tag | 7 raw IP bits after removing set-index bits |
| History line field | 24-bit line address |
| History timestamp | 16-bit timestamp |
| Delta table | 16 fully associative IP entries |
| Deltas per IP | 16 |
| Delta tag | 10-bit hashed IP tag |
| Delta field | 13-bit signed delta range |
| Learning counter | 4-bit, 16-search learning rounds |
| L1D shadow | 768 direct-mapped entries |
| Stored latency | 12-bit latency, overflow becomes zero |
| In-flight tracking | 32 entries, matching 16 MSHR + 16 PQ |

### 2. Latency-Based Training

Berti trains from two mutually exclusive paths:

1. **Demand miss fill**
   - Record the demand issue cycle.
   - Compute latency when the line fills.
   - Search history for timely deltas.
   - Update the delta table.

2. **Useful prefetched-line hit**
   - Store prefetch latency in the L1D shadow on prefetch fill.
   - Consume that latency when demand later uses the prefetched line.
   - Train once, then clear the shadow entry.

Late prefetches are guarded so that a line does not train twice when demand and
prefetch are in flight for the same line.

### 3. Timely Delta Search

For each training event:

```text
ready_cycle = fill_cycle - observed_latency
```

Berti searches same-IP history entries whose timestamps are older than or equal
to `ready_cycle`. It collects the youngest timely deltas first, with these
filters:

- At most 8 timely deltas per search.
- Delta 0 is ignored.
- Deltas outside the 13-bit signed paper range are ignored.
- Duplicate deltas within one search are ignored.

### 4. Delta Learning and Classification

Each IP entry learns for 16 searches. At the end of a learning round:

| Coverage | Status |
|---|---|
| `> 10/16` | `l1_pref` |
| `>= 8/16` | `l2_pref` |
| `> 5/16` | `l2_pref_repl` |
| otherwise | `none` |

Only `l1_pref` and `l2_pref` are issued as prefetches. `l2_pref_repl` is kept as
a weak/replacement candidate, matching the evaluated design where low/LLC-only
prefetching is disabled.

At most 12 selected deltas are retained for issue.

### 5. Delta Replacement Policy

New deltas first use empty slots. If the IP entry is full, a new delta may only
replace a weak candidate:

- `none`
- `l2_pref_repl`

If no weak candidate exists, the new delta is discarded. This keeps strong
learned deltas stable.

### 6. Prefetch Issue

On every L1D demand access:

1. Look up the IP in the delta table.
2. Select `l1_pref` and `l2_pref` deltas.
3. Add each delta to the current line.
4. Avoid already in-flight prefetches.
5. Issue the prefetch:
   - `l1_pref` fills L1D only when L1D MSHR occupancy is below 70%.
   - Otherwise the prefetch fills at the lower level.

### 7. Warmup Behavior

Before an IP entry has completed its first classification round, Berti can issue
warmup prefetches when:

- At least 8 searches have occurred.
- A delta has at least 80% confidence within the current partial round.

Warmup behavior is isolated behind a constant so it can be disabled for
debugging.

### 8. Berti-Local Statistics

The final report prints Berti-local counters only. It does not print page-level
or tier-aware counters.

Important Berti counters include:

- `ACCESSES`
- `DEMAND_LATENCY_SAMPLES`
- `PREFETCH_LATENCY_SAMPLES`
- `HISTORY_SEARCHES`
- `TIMELY_DELTAS`
- `DELTA_TABLE_HITS`
- `DELTA_TABLE_MISSES`
- `DELTA_CLASSIFICATIONS`
- `PREFETCHES_ISSUED`
- `PREFETCH_ISSUED_L1`
- `PREFETCH_ISSUED_L2`
- `USEFUL_PREFETCHES`
- `LATE_PREFETCHES`

---

## Mapping to the Berti Paper

This section is the main guide for reading the implementation. The names in the
code are intentionally close to the paper's hardware structures.

### History Table

Code:

```text
history_entry
history_set
add_history()
find_timely_deltas()
```

Paper role:

- Remembers recent demand misses and useful prefetched-line hits.
- Provides candidate older accesses for timely-delta discovery.

Implementation details:

- 8 sets and 16 ways.
- FIFO replacement through `history_set::next_victim`.
- Set index comes from the low 3 IP bits.
- History IP tag uses the next 7 raw IP bits.
- Entries store a 24-bit line value and 16-bit timestamp.

History insertion happens only for:

```text
demand miss
useful prefetched-line hit
```

This matches the paper's history-table update path and avoids filling history
with ordinary L1D hits.

### Timely Delta Search

Code:

```text
find_timely_deltas()
history_timestamp_not_after()
history_timestamp_distance()
```

Paper role:

- Given an observed latency, find prior same-IP accesses that were early enough
  to prefetch the current line.

Implementation details:

- Computes `ready_cycle = cycle - latency`.
- Searches same-IP history entries with timestamp `<= ready_cycle`.
- Sorts candidates youngest-first.
- Returns at most 8 unique timely deltas.
- Drops zero deltas and deltas outside the 13-bit signed range.
- Uses modular timestamp arithmetic for the 16-bit history timestamp.

### Delta Table

Code:

```text
ip_delta_entry
delta_entry
get_or_allocate_delta_entry()
get_or_allocate_delta()
train()
```

Paper role:

- Tracks which deltas are repeatedly timely for a given load IP.
- Counts delta occurrences during a learning round.

Implementation details:

- 16 fully associative IP entries.
- Each IP entry stores 16 deltas.
- IP tag is a 10-bit hash.
- `search_count` is the 4-bit learning/search counter.
- `seen_this_round` is the per-delta occurrence count within the current round.
- One search increments `search_count` once, even if no timely deltas are found.
- A delta can be incremented at most once per search because timely deltas are
  deduplicated before training.

### Classification

Code:

```text
classify()
limit_selected_deltas()
delta_status
```

Paper role:

- Converts learned coverage into prefetch placement status.

Implementation details:

After 16 searches:

| Code condition | Status | Meaning |
|---|---|---|
| `coverage > 10` | `l1_pref` | High coverage, eligible for L1D fill |
| `coverage >= 8` | `l2_pref` | Medium coverage, lower-level fill |
| `coverage > 5` | `l2_pref_repl` | Weak L2 candidate, replacement only |
| otherwise | `none` | Not selected |

Only `l1_pref` and `l2_pref` issue prefetches. `l2_pref_repl` is retained as a
weak candidate so it can be replaced by newer deltas.

The selected set is capped at 12 deltas per IP entry.

### Replacement Policy

Code:

```text
find_delta_replacement()
replaceable_delta_status()
```

Paper role:

- Protect strong deltas while allowing weak or unused candidates to be replaced.

Implementation details:

- Empty slots are used first.
- If full, only `none` or `l2_pref_repl` entries are replaceable.
- If every delta is strong, the new delta is discarded.

### Latency Tracking

Code:

```text
in_flight_entry
prefetch_latency_entry
record_demand_issue()
record_prefetch_issue()
remember_prefetch_latency()
consume_prefetch_latency()
prefetcher_cache_fill()
```

Paper role:

- Berti depends on measured latency to decide which earlier deltas were timely.

Implementation details:

- Demand issue cycle is recorded by line.
- Prefetch issue cycle is recorded by line.
- Demand fill trains immediately using demand latency.
- Prefetch fill stores latency in the L1D shadow.
- A later useful-prefetch demand hit consumes that stored latency and trains once.
- If demand and prefetch are both in flight for the same line, the demand-fill
  path trains and the prefetch shadow entry is not created. This avoids
  double-training late prefetches.

### Prefetch Issue

Code:

```text
issue_prefetches()
selected_deltas()
target_line()
prefetch_line()
```

Paper role:

- Uses selected deltas for the current IP to prefetch future lines.

Implementation details:

- Runs on every L1D demand access.
- Uses selected `l1_pref` and `l2_pref` deltas.
- Computes `target = current_line + delta`.
- Skips already in-flight prefetches.
- `l1_pref` fills L1D only when L1D MSHR occupancy is below 70%.
- Otherwise the prefetch is issued as a lower-level fill.

### Warmup

Code:

```text
add_warmup_deltas()
warmup_prefetching_ready()
```

Paper role:

- Allows early prefetching before the first full 16-search learning round.

Implementation details:

- Warmup is enabled only before the IP entry has been classified.
- Requires at least 8 searches.
- Requires at least 80% confidence.
- Warmup candidates are treated as high-confidence L1D prefetches.

---

## Configuration

The Berti branch uses:

```text
tiered_memory_config.json
```

The relevant settings are:

```json
{
  "executable_name": "champsim_berti_tiered",
  "L1D": {
    "pq_size": 16,
    "mshr_size": 16,
    "virtual_prefetch": true,
    "prefetcher": "berti"
  },
  "virtual_memory": {
    "randomization": false
  }
}
```

The DDR+CXL tiered memory model remains enabled so this branch can evaluate how
ordinary, non-tier-aware Berti behaves in a tiered system.

---

## Build

Install dependencies as usual for ChampSim:

```bash
git submodule update --init
vcpkg/bootstrap-vcpkg.sh
vcpkg/vcpkg install
```

Configure and build the Berti-tiered binary:

```bash
./config.sh tiered_memory_config.json
make -j"$(nproc)"
```

Expected binary:

```text
bin/champsim_berti_tiered
```

---

## Run

Use `stdbuf -oL` when piping through `tee`; otherwise output may be fully
buffered and the result file may remain empty until the simulator flushes.

Example short validation run:

```bash
stdbuf -oL ./bin/champsim_berti_tiered \
  --warmup-instructions 10000000 \
  --simulation-instructions 50000000 \
  traces/605.mcf_s-1152B.champsimtrace.xz \
  | tee results_berti_tiered_mcf_10M_50M.txt
```

For a no-prefetch tiered comparison, create a temporary config that changes only
the executable name and L1D prefetcher:

```bash
jq '.executable_name = "champsim_tiered_nopf" | .L1D.prefetcher = "no"' \
  tiered_memory_config.json > tiered_memory_nopf_config.json

./config.sh tiered_memory_nopf_config.json
make -j"$(nproc)"

stdbuf -oL ./bin/champsim_tiered_nopf \
  --warmup-instructions 10000000 \
  --simulation-instructions 50000000 \
  traces/605.mcf_s-1152B.champsimtrace.xz \
  | tee results_tiered_nopf_mcf_10M_50M.txt
```

Do not commit temporary result files unless they are intentionally part of a
report artifact.

---

## Interpreting Output

### 1. Confirm Tiered Memory Is Active

Look for:

```text
DRAM Size: 64 MiB
CXL Size: 4 GiB
Virtual Memory DDR_CAPACITY_PAGES
Virtual Memory CXL_CAPACITY_PAGES
CXL Channel 0 READ_REQUESTS
```

If `CURRENT_CXL_PAGES` and `CXL Channel 0 READ_REQUESTS` are nonzero, the run is
exercising the CXL tier.

### 2. Confirm Berti Is Active

Look for:

```text
Berti prefetcher statistics
PREFETCHES_ISSUED
HISTORY_SEARCHES
TIMELY_DELTAS
DELTA_CLASSIFICATIONS
USEFUL_PREFETCHES
```

If `PREFETCHES_ISSUED` and `DELTA_CLASSIFICATIONS` are nonzero, Berti is
learning and issuing prefetches.

### 3. Compare Against No Prefetch

Useful comparison metrics:

```text
CPU 0 cumulative IPC
CPU cycles
L1D LOAD MISS
L1D PREFETCH REQUESTED / ISSUED / USEFUL
DRAM Channel 0 READ_REQUESTS
CXL Channel 0 READ_REQUESTS
DDR TIER ACCESS RATE
AVERAGE OFF-CHIP DEMAND ACCESS LATENCY
AVERAGE DDR DEMAND ACCESS LATENCY
AVERAGE CXL DEMAND ACCESS LATENCY
AVERAGE RQ OCCUPANCY
PEAK RQ OCCUPANCY
```

The non-tier-aware Berti baseline is useful if it improves IPC or reduces demand
misses. A future tier-aware Berti design should try to preserve that benefit
while reducing unnecessary CXL queue pressure, late prefetches, or bandwidth
waste.

---

## References

If you use ChampSim, cite:

```text
Gober, N., Chacon, G., Wang, L., Gratz, P. V., Jimenez, D. A.,
Teran, E., Pugsley, S., & Kim, J. (2022).
The Championship Simulator: Architectural Simulation for Education and Competition.
https://doi.org/10.48550/arXiv.2210.14324
```

This branch is intended to support experiments around Berti-style prefetching in
tiered DDR+CXL memory systems.
