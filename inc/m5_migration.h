/*
 * M5 migration mechanics — cost model and statistics.
 *
 * Migration cost is computed from configured page-copy bandwidth and is
 * accumulated as a stat rather than modeled as explicit CPU stalls.
 * This is the first-version simplification: cost is nonzero and tracked,
 * and placement updates take effect immediately after migrate() is called.
 */

#ifndef M5_MIGRATION_H
#define M5_MIGRATION_H

#include <cstdint>
#include <cstddef>

namespace m5
{

// Per-simulation migration accounting.
// M5Manager owns one of these and passes it to the printer in Step 9.
struct MigrationStats {
  uint64_t total_promotions           = 0;  // CXL → DDR
  uint64_t total_demotions            = 0;  // DDR → CXL (one per promotion when DDR is full)
  uint64_t total_migration_cost_cycles= 0;  // accumulated CPU cycles spent copying pages

  // Counters for pages that were considered but skipped each epoch.
  uint64_t skipped_density_filter     = 0;  // HWT density below hwt_density_threshold
  uint64_t skipped_bw_density_trigger = 0;  // CXL/DDR density ratio below threshold
  uint64_t skipped_cooldown           = 0;  // page still in migration cooldown
  uint64_t skipped_no_victim          = 0;  // MGLRU found no eligible DDR victim

  uint64_t total_epochs_fired         = 0;  // epochs where migration pipeline ran
  uint64_t total_epochs_skipped       = 0;  // epochs where Elector decided not to migrate
};

// Compute the CPU-cycle cost of copying one page between tiers.
//
// page_size_bytes       : configured page size (default 4096)
// migration_bw_GBps     : configured copy bandwidth in GB/s (default 10.0)
// cpu_frequency_MHz     : CPU clock in MHz (default 4000 for 4 GHz)
//
// Example: 4096 B / (10 GB/s) = 409.6 ns × 4 GHz = 1638 cycles.
inline uint64_t compute_migration_cost_cycles(std::size_t page_size_bytes,
                                               double      migration_bw_GBps,
                                               uint64_t    cpu_frequency_MHz)
{
  double cost_seconds = static_cast<double>(page_size_bytes)
                        / (migration_bw_GBps * 1.0e9);
  double cycles = cost_seconds * static_cast<double>(cpu_frequency_MHz) * 1.0e6;
  return static_cast<uint64_t>(cycles);
}

} // namespace m5

#endif // M5_MIGRATION_H
