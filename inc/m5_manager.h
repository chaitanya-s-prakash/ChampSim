/*
 * M5Manager — epoch-driven migration pipeline.
 *
 * Orchestrates the four M5 policy roles every migration_epoch_instrs
 * retired CPU instructions:
 *
 *   Monitor   → compute DDR/CXL bandwidth density via M5Monitor
 *   Nominator → filter HPT candidates through HWT density check
 *   Elector   → decide whether to migrate based on density ratio
 *   Promoter  → swap placement slots, account migration cost
 *
 * Triggered from O3_CPU::operate() via the global g_m5_manager pointer.
 * Initialized in champsim::main() before the first simulation phase.
 */

#ifndef M5_MANAGER_H
#define M5_MANAGER_H

#include <cstdint>
#include <vector>

#include "dram_controller.h"
#include "m5_migration.h"
#include "m5_monitor.h"
#include "m5_policy.h"

namespace m5
{

class M5Manager
{
  MEMORY_CONTROLLER& dram_;
  const config       cfg_;

  MigrationStats stats_{};           // cumulative since simulation start (incl. warmup)
  MigrationStats roi_snapshot_{};   // snapshot taken at warmup→ROI transition
  uint64_t       last_epoch_instr_ = 0;
  uint64_t       epoch_count_      = 0;  // total epochs run so far (for aging interval)

  // Pipeline stages --------------------------------------------------------
  // Nominator: returns logical page numbers of filtered CXL promotion candidates.
  std::vector<uint64_t> nominate_candidates();

  // Elector: returns true when migration should proceed this epoch.
  bool should_migrate(const EpochStats& epoch_stats, std::size_t n_candidates);

  // Promoter: execute up to max_migrations_per_epoch promotions.
  void promote_candidates(const std::vector<uint64_t>& candidates);

  // Reset HPT/HWT and age MGLRU generations after each epoch.
  void reset_and_age();

public:
  M5Manager(MEMORY_CONTROLLER& dram, const config& cfg = {});

  // Called from O3_CPU::operate() every time instructions are retired.
  // Fires the full pipeline when enough instructions have elapsed.
  void maybe_trigger_epoch(uint64_t num_retired);

  // Call at the warmup → ROI transition to mark the start of the measurement
  // window.  roi_migration_stats() then returns only post-snapshot activity.
  void begin_roi();

  // Returns migration stats accumulated since the last begin_roi() call.
  MigrationStats roi_migration_stats() const;

  const MigrationStats& sim_migration_stats() const { return stats_; }
};

// Global singleton set in champsim::main() before any phase runs.
// ooo_cpu.cc reads this pointer to call maybe_trigger_epoch().
extern M5Manager* g_m5_manager;

} // namespace m5

#endif // M5_MANAGER_H
