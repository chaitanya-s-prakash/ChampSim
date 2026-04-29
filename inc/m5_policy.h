/*
 * M5 page migration policy — configuration parameters.
 *
 * All M5 knobs live here so they can be swept in sensitivity experiments
 * without touching any other source file.
 */

#ifndef M5_POLICY_H
#define M5_POLICY_H

#include <cstddef>
#include <cstdint>

namespace m5
{

struct config {

  /* -- address granularity ------------------------------------------------ */
  std::size_t page_size      = 4096;  // bytes
  std::size_t cacheline_size = 64;    // bytes
  // lines_per_page = page_size / cacheline_size  (derived, not stored)

  /* -- tier capacity (set at init from MEMORY_CONTROLLER if left at 0) ---- */
  uint64_t ddr_capacity_pages = 0;
  uint64_t cxl_capacity_pages = 0;

  /* -- CXL extra latency -------------------------------------------------- */
  // Fixed picoseconds added to every CXL access on top of DRAM timing.
  // Leave at 0 when tCAS/tRCD/tRP already encode the full CXL penalty.
  uint64_t cxl_extra_latency_ps = 0;

  /* -- migration cost ----------------------------------------------------- */
  // Effective copy bandwidth (GB/s) between tiers.
  // Migration delay = page_size / (migration_bandwidth_GBps * 1e9) seconds,
  // converted to simulator picoseconds.
  double migration_bandwidth_GBps = 10.0;

  /* -- manager intervals -------------------------------------------------- */
  // Manager pipeline runs every this many retired instructions.
  uint64_t migration_epoch_instrs = 10'000'000;

  // HPT/HWT reset interval in retired instructions.
  // 0 means reset at every migration epoch.
  uint64_t tracker_reset_instrs = 0;

  /* -- HPT (Hot Page Tracker) --------------------------------------------- */
  std::size_t hpt_cms_width = 8192;   // counters per CMS row
  std::size_t hpt_cms_depth = 4;      // CMS rows (hash functions)
  std::size_t hpt_top_k     = 5;     // max hot-page candidates per epoch

  /* -- HWT (Hot Word / Cache-Line Tracker) -------------------------------- */
  std::size_t hwt_cms_width         = 8192;  // counters per CMS row
  std::size_t hwt_cms_depth         = 4;     // CMS rows
  std::size_t hwt_top_k             = 5;   // max hot cache-line candidates per epoch
  std::size_t hwt_density_threshold = 8;     // min hot lines in a page to qualify for migration

  /* -- Elector / bandwidth-density trigger -------------------------------- */
  // Migrate only when cxl_bw_density / ddr_bw_density >= this value.
  // 0.0 = always migrate when candidates exist.
  double bw_density_ratio_threshold = 1.0;

  std::size_t max_migrations_per_epoch = 5;  // promotion limit per epoch
  std::size_t min_candidates_per_epoch = 1;  // HPT must report at least this many

  /* -- MGLRU-like victim selection ---------------------------------------- */
  std::size_t mglru_generations           = 4;  // generation count (0=hot, N-1=cold)
  std::size_t mglru_aging_interval_epochs = 1;  // epochs between aging passes

  /* -- anti-ping-pong cooldown -------------------------------------------- */
  // Epochs a page must wait after migration before it can be migrated again.
  std::size_t migration_cooldown_epochs = 2;
};

} // namespace m5

#endif // M5_POLICY_H
