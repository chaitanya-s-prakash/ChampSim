#include "m5_manager.h"

#include <algorithm>
#include <cstdint>

#include "champsim.h"       // LOG2_PAGE_SIZE, LOG2_BLOCK_SIZE, PAGE_SIZE, BLOCK_SIZE
#include "page_placement.h"

namespace m5
{

// Global singleton — set to a live instance by champsim::main() before any
// phase runs, and reset to nullptr after phases complete.
M5Manager* g_m5_manager = nullptr;

// ---------------------------------------------------------------------------
M5Manager::M5Manager(MEMORY_CONTROLLER& dram, const config& cfg)
    : dram_(dram), cfg_(cfg)
{
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
