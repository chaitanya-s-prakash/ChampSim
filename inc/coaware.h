/*
 * Co-Aware M5 + Berti — shared state header.
 *
 * This file is the single source of truth for all data structures and
 * constants shared between:
 *   - Berti (L1D prefetcher)  — populates ip_page_access_count, ip_max_coverage
 *   - M5Manager (DRAM ctrl)  — populates page_uncovered_rate, page_prefetchability,
 *                               page_hotness, page_total_accesses; reads all of the above
 *
 * Design notes:
 *   - Tier membership is NOT duplicated here.  The authoritative source is
 *     PagePlacementTable inside MEMORY_CONTROLLER.  Use g_mc->is_cxl_access()
 *     or g_mc->placement_table.get_tier() for all tier checks.
 *   - Cross-module signalling (Berti invalidation on migration) is done via
 *     recently_migrated_pages / recently_demoted_pages — plain maps written by
 *     M5Manager and read by Berti on the next History Table write.  No function
 *     calls cross module boundaries (Fix C1).
 *   - ip_page_access_count and ip_max_coverage are keyed on
 *     delta_table_ip_tag (10-bit hash), consistent with Berti's delta table.
 */

#ifndef COAWARE_H
#define COAWARE_H

#include <cstddef>
#include <cstdint>
#include <unordered_map>

class MEMORY_CONTROLLER;  // forward declaration — avoids circular include with dram_controller.h

namespace coaware {

// ── Populated by Berti learning path ─────────────────────────────────────────
// Key: delta_table_ip_tag (10-bit), matches Berti's delta table identity.
// Inner key: logical page number.  Value: demand access count this epoch.
// Fully cleared at every epoch boundary (per-epoch semantics, Fix M3).
extern std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> ip_page_access_count;

// Key: delta_table_ip_tag.  Value: max(delta.coverage / LEARNING_ROUNDS) across
// all valid deltas for that ip_tag.  Updated by Berti's classify() every 16 searches.
// NOT reset at epoch boundary — persists until Berti rewrites it (Fix M3).
extern std::unordered_map<uint64_t, float> ip_max_coverage;

// ── Populated by MEMORY_CONTROLLER::add_rq ───────────────────────────────────
// Key: logical page number.

// Per-page hotness: incremented on every demand DRAM read, decayed each epoch.
// Tracks DRAM-level traffic (not L1D hits) (Fix C4).
extern std::unordered_map<uint64_t, float> page_hotness;

// Per-page lifetime access count: monotonically increasing, never reset.
// Used only for the grace-period check (Fix M1).
extern std::unordered_map<uint64_t, uint64_t> page_total_accesses;

// ── Computed at epoch boundary by M5Manager ──────────────────────────────────
// Key: logical page number.

// Uncovered access rate: Σ freq*(1-coverage) summed over IPs that accessed this
// page in the epoch.  Non-zero only for CXL-resident pages.
// Cleared and recomputed each epoch.
extern std::unordered_map<uint64_t, float> page_uncovered_rate;

// Weighted-average prefetchability for DDR-resident pages.
// Used for co-aware demotion scoring.  NOT reset each epoch (Fix M3).
extern std::unordered_map<uint64_t, float> page_prefetchability;

// ── Migration signals — cross-module communication (Fix C1) ──────────────────
// Written by M5Manager after placement_table.migrate(); read by Berti.
// Value: epoch_count_ at time of migration.
// Signal is valid while (current_epoch - value) <= 1 (roughly one epoch lifetime).

// Both promoted (CXL→DDR) and demoted (DDR→CXL) pages appear here.
// Berti skips History Table writes for pages in this map (forces latency re-learning).
extern std::unordered_map<uint64_t, uint64_t> recently_migrated_pages;

// Only demoted (DDR→CXL) pages appear here.
// Berti uses this to downgrade l1_pref → l2_pref during the CXL re-learning window.
extern std::unordered_map<uint64_t, uint64_t> recently_demoted_pages;

// ── Secondary promotion trigger state (Fix C3) ────────────────────────────────
// Key: logical page number (CXL-resident).
// Value: consecutive epochs where page_uncovered_rate exceeded the threshold.
// Reset to 0 when rate drops below threshold, or when the page is promoted.
extern std::unordered_map<uint64_t, int> high_uncovered_rate_epochs;

// Per-page count of CXL-targeted prefetches issued by Berti this epoch.
// Key: logical page number.  Value: prefetch issue count.
// Written by Berti's issue path; reset at every epoch boundary.
// Used to rank candidates under Trigger 2 (budget exhaustion): the pages Berti
// is actively prefetching from CXL are the ones consuming the budget, so they
// are the correct targets to promote — not the demand-miss-heavy pages.
extern std::unordered_map<uint64_t, uint64_t> cxl_pf_issued_per_page;

// ── Epoch-level state ─────────────────────────────────────────────────────────

// Mirrors M5Manager::epoch_count_.  Set at the start of each epoch.
extern uint64_t current_epoch;

// Maximum page_hotness value observed after decay, recomputed each epoch (Fix M4).
// Used for normalized hotness in demotion scoring.  Initialized to 1.0 to
// prevent divide-by-zero on the first epoch.
extern float epoch_max_hotness;

// CXL prefetch budget: decremented by Berti on each CXL-targeted prefetch.
// Reset to CXL_PREFETCH_BUDGET_PER_EPOCH at each epoch boundary.
extern int cxl_prefetch_budget_remaining;

// Consecutive epochs where cxl_prefetch_budget_remaining reached 0.
// Reset when budget is not exhausted or when migration fires.
extern int cxl_prefetch_budget_exhausted_epochs;

// ── Global MEMORY_CONTROLLER pointer ─────────────────────────────────────────
// Set by champsim::main() before simulation starts; cleared after.
// Allows Berti (a separate translation unit) to query tier membership without
// a direct compile-time dependency on dram_controller.h.
extern MEMORY_CONTROLLER* g_mc;

// ── Tunable constants ─────────────────────────────────────────────────────────

// CXL prefetch budget per epoch.  Berti suppresses CXL-targeted prefetches
// once this many have been issued in the current epoch.
static constexpr int CXL_PREFETCH_BUDGET_PER_EPOCH = 64;

// Number of consecutive epochs a trigger condition must hold before promotion fires.
static constexpr int MIGRATION_TRIGGER_EPOCHS = 3;

// Secondary trigger: uncovered_access_rate threshold for a CXL page (Fix C3).
// If a page's uncovered_access_rate exceeds this for MIGRATION_TRIGGER_EPOCHS
// consecutive epochs, promotion is triggered regardless of bandwidth density.
static constexpr float UNCOVERED_RATE_MIGRATION_THRESHOLD = 50.0f;

// Maximum number of pages promoted per migration event (co-aware path).
static constexpr int MIGRATION_K = 4;

// Per-epoch hotness decay multiplier applied at each epoch boundary.
static constexpr float HOTNESS_DECAY_FACTOR = 0.5f;

// Minimum lifetime DRAM accesses before a page's prefetchability signal is
// trusted.  During grace period, prefetchability is set to 0.5 (neutral) (Fix M1).
static constexpr uint64_t GRACE_PERIOD_ACCESSES = 100;

// Maximum pages tracked per IP in ip_page_access_count.  Entries beyond this
// are evicted by minimum-access-count policy (Fix M2).
static constexpr size_t MAX_PAGES_PER_IP = 32;

// Demotion score thresholds for bucket assignment.
// score = prefetchability * (1 - normalized_hotness)
static constexpr float BUCKET_A_THRESHOLD = 0.66f;  // demote first
static constexpr float BUCKET_B_THRESHOLD = 0.33f;  // demote second

} // namespace coaware

#endif // COAWARE_H
