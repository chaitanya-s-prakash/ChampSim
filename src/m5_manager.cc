#include "m5_manager.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

#include "champsim.h"       // LOG2_PAGE_SIZE, LOG2_BLOCK_SIZE, PAGE_SIZE, BLOCK_SIZE
#include "coaware.h"
#include "page_placement.h"

namespace m5
{

// ---------------------------------------------------------------------------
// Co-aware signal computation helpers — called at every epoch boundary.
// Both helpers are file-scoped (static) because only M5Manager needs them.
// ---------------------------------------------------------------------------

// Compute page_uncovered_rate for every CXL-resident page.
// Formula: for each (ip_tag, page) pair in ip_page_access_count where the
// page is CXL-resident, add freq * (1 - coverage) to page_uncovered_rate.
// Pages that have not yet accumulated GRACE_PERIOD_ACCESSES total lifetime
// DRAM reads are skipped — the prefetchability signal is not yet trustworthy.
static void compute_uncovered_access_rates(MEMORY_CONTROLLER& dram)
{
  coaware::page_uncovered_rate.clear();

  for (const auto& [ip_tag, page_map] : coaware::ip_page_access_count) {
    const float coverage = coaware::ip_max_coverage.count(ip_tag)
                           ? coaware::ip_max_coverage.at(ip_tag) : 0.0f;
    const float uncov = 1.0f - coverage;

    for (const auto& [page_num, freq] : page_map) {
      // Grace period: skip pages with insufficient DRAM access history (Fix M1).
      const uint64_t lifetime = coaware::page_total_accesses.count(page_num)
                                ? coaware::page_total_accesses.at(page_num) : 0;
      if (lifetime < coaware::GRACE_PERIOD_ACCESSES)
        continue;

      champsim::address addr{page_num << LOG2_PAGE_SIZE};
      if (dram.placement_table.get_tier(addr) == PagePlacementTable::Tier::CXL)
        coaware::page_uncovered_rate[page_num] += static_cast<float>(freq) * uncov;
    }
  }
}

// Compute page_prefetchability for every DDR-resident page.
// Formula: weighted average of ip_max_coverage across all IPs that accessed
// the page this epoch, weighted by access frequency.
// Pages still in the grace period get a neutral value of 0.5 (Fix M1) so
// they neither strongly attract nor repel demotion decisions.
static void update_page_prefetchability(MEMORY_CONTROLLER& dram)
{
  std::unordered_map<uint64_t, float>    total_acc;
  std::unordered_map<uint64_t, float>    weighted_cov;

  for (const auto& [ip_tag, page_map] : coaware::ip_page_access_count) {
    const float cov = coaware::ip_max_coverage.count(ip_tag)
                      ? coaware::ip_max_coverage.at(ip_tag) : 0.0f;

    for (const auto& [page_num, freq] : page_map) {
      champsim::address addr{page_num << LOG2_PAGE_SIZE};
      if (dram.placement_table.get_tier(addr) != PagePlacementTable::Tier::DDR)
        continue;
      total_acc[page_num]    += static_cast<float>(freq);
      weighted_cov[page_num] += static_cast<float>(freq) * cov;
    }
  }

  for (const auto& [page_num, total] : total_acc) {
    const uint64_t lifetime = coaware::page_total_accesses.count(page_num)
                              ? coaware::page_total_accesses.at(page_num) : 0;
    if (lifetime < coaware::GRACE_PERIOD_ACCESSES)
      coaware::page_prefetchability[page_num] = 0.5f;   // neutral during grace period
    else if (total > 0.0f)
      coaware::page_prefetchability[page_num] = weighted_cov.at(page_num) / total;
    // If total == 0 (shouldn't happen) leave existing value — staleness accepted.
  }
}

// Global singleton — set to a live instance by champsim::main() before any
// phase runs, and reset to nullptr after phases complete.
M5Manager* g_m5_manager = nullptr;

// ---------------------------------------------------------------------------
M5Manager::M5Manager(MEMORY_CONTROLLER& dram, const config& cfg)
    : dram_(dram), cfg_(cfg)
{
}

void M5Manager::begin_roi()
{
  // Snapshot current cumulative stats.  roi_migration_stats() returns the
  // delta (sim_stats - snapshot), matching the DRAM channel roi_stats pattern.
  roi_snapshot_ = stats_;
}

MigrationStats M5Manager::roi_migration_stats() const
{
  MigrationStats roi;
  roi.total_epochs_fired          = stats_.total_epochs_fired          - roi_snapshot_.total_epochs_fired;
  roi.total_epochs_skipped        = stats_.total_epochs_skipped        - roi_snapshot_.total_epochs_skipped;
  roi.total_promotions            = stats_.total_promotions            - roi_snapshot_.total_promotions;
  roi.total_demotions             = stats_.total_demotions             - roi_snapshot_.total_demotions;
  roi.total_migration_cost_cycles = stats_.total_migration_cost_cycles - roi_snapshot_.total_migration_cost_cycles;
  roi.skipped_density_filter      = stats_.skipped_density_filter      - roi_snapshot_.skipped_density_filter;
  roi.skipped_bw_density_trigger  = stats_.skipped_bw_density_trigger  - roi_snapshot_.skipped_bw_density_trigger;
  roi.skipped_cooldown            = stats_.skipped_cooldown            - roi_snapshot_.skipped_cooldown;
  roi.skipped_no_victim           = stats_.skipped_no_victim           - roi_snapshot_.skipped_no_victim;
  return roi;
}

// ---------------------------------------------------------------------------
// Nominator: filter HPT top-K by tier, cooldown, and HWT density.
// ---------------------------------------------------------------------------
std::vector<uint64_t> M5Manager::nominate_candidates()
{
  const uint64_t lines_per_page = cfg_.page_size / cfg_.cacheline_size;
  const auto&    hpt_entries    = dram_.hpt.top_entries();
  const auto&    hwt_entries    = dram_.hwt.top_entries();

  std::vector<uint64_t> candidates;

  for (const auto& hpt_e : hpt_entries) {
    uint64_t ppage = hpt_e.key;

    // Page must still be CXL-resident (could have been promoted earlier in epoch).
    champsim::address addr{ppage << LOG2_PAGE_SIZE};
    if (dram_.placement_table.get_tier(addr) != PagePlacementTable::Tier::CXL)
      continue;

    // Skip pages still in anti-ping-pong cooldown.
    if (dram_.placement_table.is_in_cooldown(ppage)) {
      ++stats_.skipped_cooldown;
      continue;
    }

    // HWT density check: count how many distinct hot cache lines in this page
    // appear in the HWT top-K.  A threshold of 0 disables the filter entirely.
    if (cfg_.hwt_density_threshold > 0) {
      std::size_t hot_lines = 0;
      for (const auto& hwt_e : hwt_entries) {
        if (hwt_e.key / lines_per_page == ppage)
          ++hot_lines;
      }
      if (hot_lines < cfg_.hwt_density_threshold) {
        ++stats_.skipped_density_filter;
        continue;
      }
    }

    candidates.push_back(ppage);
  }

  return candidates;
}

// ---------------------------------------------------------------------------
// Elector: decide whether migration is beneficial this epoch.
// ---------------------------------------------------------------------------
bool M5Manager::should_migrate(const EpochStats& es, std::size_t n_candidates)
{
  if (n_candidates < cfg_.min_candidates_per_epoch)
    return false;

  // Bandwidth-density trigger: CXL is "hotter per page" than DDR.
  if (es.density_ratio < cfg_.bw_density_ratio_threshold) {
    ++stats_.skipped_bw_density_trigger;
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Promoter: execute migrations and account for copy cost.
// ---------------------------------------------------------------------------
void M5Manager::promote_candidates(const std::vector<uint64_t>& candidates)
{
  const uint8_t  max_gen       = static_cast<uint8_t>(cfg_.mglru_generations - 1);
  const uint64_t cost_per_page = compute_migration_cost_cycles(
      cfg_.page_size, cfg_.migration_bandwidth_GBps,
      4000); // assumes 4 GHz CPU; adjust if config exposes cpu_frequency_MHz

  std::size_t done = 0;

  for (uint64_t cxl_ppage : candidates) {
    if (done >= cfg_.max_migrations_per_epoch)
      break;

    uint64_t ddr_victim = dram_.placement_table.get_ddr_victim(max_gen);
    if (ddr_victim == UINT64_MAX) {
      ++stats_.skipped_no_victim;
      break; // no eligible DDR victim — stop for this epoch
    }

    dram_.placement_table.migrate(cxl_ppage, ddr_victim);
    dram_.placement_table.set_cooldown(cxl_ppage,
                                       static_cast<uint32_t>(cfg_.migration_cooldown_epochs));
    dram_.placement_table.set_cooldown(ddr_victim,
                                       static_cast<uint32_t>(cfg_.migration_cooldown_epochs));

    ++stats_.total_promotions;
    ++stats_.total_demotions;
    stats_.total_migration_cost_cycles += cost_per_page;
    ++done;
  }
}

// ---------------------------------------------------------------------------
// Reset HPT/HWT trackers and run MGLRU aging pass.
// ---------------------------------------------------------------------------
void M5Manager::reset_and_age()
{
  dram_.hpt.reset();
  dram_.hwt.reset();

  if (epoch_count_ % cfg_.mglru_aging_interval_epochs == 0) {
    const uint8_t max_gen = static_cast<uint8_t>(cfg_.mglru_generations - 1);
    dram_.placement_table.age_all_ddr_pages(max_gen);
  }
}

// ---------------------------------------------------------------------------
// Main entry point — called from O3_CPU::operate() every cycle.
// ---------------------------------------------------------------------------
void M5Manager::maybe_trigger_epoch(uint64_t num_retired)
{
  if (num_retired < last_epoch_instr_ + cfg_.migration_epoch_instrs)
    return;

  last_epoch_instr_ = num_retired;
  ++epoch_count_;
  ++stats_.total_epochs_fired;

  // ── 0. Co-aware epoch bookkeeping ─────────────────────────────────────────
  // Must run before the M5 pipeline so the Nominator and Elector see fresh
  // co-aware signals, and before ip_page_access_count is cleared.

  // Expose current epoch number to Berti (used for migration signal expiry).
  coaware::current_epoch = epoch_count_;

  // Compute co-aware signals from this epoch's access data.
  compute_uncovered_access_rates(dram_);
  update_page_prefetchability(dram_);

  // Fix M4: capture max hotness BEFORE decay so normalisation is consistent
  // within this epoch.  Initialise to 1.0 to prevent divide-by-zero when all
  // pages have zero hotness (e.g. first epoch).
  coaware::epoch_max_hotness = 1.0f;
  for (const auto& [pn, h] : coaware::page_hotness)
    if (h > coaware::epoch_max_hotness) coaware::epoch_max_hotness = h;

  // Fix M3 — DECAY: page_hotness carries weighted history across epochs.
  for (auto& [pn, h] : coaware::page_hotness)
    h *= coaware::HOTNESS_DECAY_FACTOR;

  // Fix M3 — FULL CLEAR: ip_page_access_count is per-epoch only.
  // ip_max_coverage is NOT cleared — it persists until Berti rewrites it.
  for (auto& [ip, pm] : coaware::ip_page_access_count)
    pm.clear();

  // CXL prefetch budget: track consecutive exhausted epochs (Trigger 2),
  // then reset for the new epoch.
  if (coaware::cxl_prefetch_budget_remaining <= 0)
    ++coaware::cxl_prefetch_budget_exhausted_epochs;
  else
    coaware::cxl_prefetch_budget_exhausted_epochs = 0;
  coaware::cxl_prefetch_budget_remaining = coaware::CXL_PREFETCH_BUDGET_PER_EPOCH;

  // Fix C3: per-page consecutive epoch counter for high uncovered-access-rate.
  // Increment for pages above the threshold; reset for pages that dropped below.
  for (const auto& [page_num, rate] : coaware::page_uncovered_rate) {
    if (rate > coaware::UNCOVERED_RATE_MIGRATION_THRESHOLD)
      ++coaware::high_uncovered_rate_epochs[page_num];
    else
      coaware::high_uncovered_rate_epochs[page_num] = 0;
  }

  // Fix C3: also reset counter for any page no longer in page_uncovered_rate
  // (it was promoted to DDR this epoch or dropped out of tracking).
  for (auto& [page_num, cnt] : coaware::high_uncovered_rate_epochs) {
    if (!coaware::page_uncovered_rate.count(page_num))
      cnt = 0;
  }

  // Expire migration signals older than one epoch so the maps don't grow
  // indefinitely.  Berti treats signals with age > 1 as already expired, but
  // we clean up here to bound memory use.
  for (auto it = coaware::recently_migrated_pages.begin();
       it != coaware::recently_migrated_pages.end(); ) {
    it = (epoch_count_ - it->second > 1)
         ? coaware::recently_migrated_pages.erase(it)
         : std::next(it);
  }
  for (auto it = coaware::recently_demoted_pages.begin();
       it != coaware::recently_demoted_pages.end(); ) {
    it = (epoch_count_ - it->second > 1)
         ? coaware::recently_demoted_pages.erase(it)
         : std::next(it);
  }

  // Fix S8: reset per-page CXL prefetch issue count for the new epoch.
  coaware::cxl_pf_issued_per_page.clear();

  // ── 1. Monitor ────────────────────────────────────────────────────────────
  uint64_t ddr_bytes = 0, cxl_bytes = 0;
  const std::size_t n_primary = dram_.num_primary_channels();
  for (std::size_t i = 0; i < dram_.channels.size(); ++i) {
    uint64_t b = dram_.channels[i].sim_stats.bytes_returned;
    if (i < n_primary)
      ddr_bytes += b;
    else
      cxl_bytes += b;
  }
  auto epoch_stats = dram_.monitor.reset_epoch(
      ddr_bytes, cxl_bytes,
      dram_.placement_table.ddr_pages_used(),
      dram_.placement_table.cxl_pages_used());

  // ── 2. Nominator ─────────────────────────────────────────────────────────
  auto candidates = nominate_candidates();

  // ── 3. Elector ───────────────────────────────────────────────────────────
  if (!should_migrate(epoch_stats, candidates.size())) {
    ++stats_.total_epochs_skipped;
    reset_and_age();
    return;
  }

  // ── 4. Promoter ──────────────────────────────────────────────────────────
  promote_candidates(candidates);

  // ── 5. Reset trackers and age MGLRU ──────────────────────────────────────
  reset_and_age();
}

} // namespace m5
