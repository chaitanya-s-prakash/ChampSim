/*
 * M5Monitor — per-epoch bandwidth and density tracker.
 *
 * Sits between the M5Manager (Step 8) and the DRAM channels.
 * The caller passes in pre-computed cumulative byte counts; this class
 * computes per-epoch deltas and bandwidth densities without needing to
 * know anything about DRAM_CHANNEL internals.
 *
 * Bandwidth density = bytes served / pages resident in that tier.
 * The ratio CXL_density / DDR_density is the Elector's primary trigger.
 */

#ifndef M5_MONITOR_H
#define M5_MONITOR_H

#include <cstdint>

namespace m5
{

struct EpochStats {
  uint64_t ddr_read_bytes = 0;  // demand bytes returned from DDR this epoch
  uint64_t cxl_read_bytes = 0;  // demand bytes returned from CXL this epoch
  uint64_t ddr_pages      = 0;  // DDR-resident pages at epoch boundary
  uint64_t cxl_pages      = 0;  // CXL-resident pages at epoch boundary
  double   ddr_bw_density = 0.0; // ddr_read_bytes / ddr_pages
  double   cxl_bw_density = 0.0; // cxl_read_bytes / cxl_pages
  double   density_ratio  = 0.0; // cxl_bw_density / ddr_bw_density (0 if ddr=0)
  uint64_t epoch_index    = 0;
};

class M5Monitor
{
  // Cumulative bytes_returned snapshots at the end of the previous epoch.
  // Delta = current - snapshot = this epoch's bytes.
  uint64_t snap_ddr_bytes_ = 0;
  uint64_t snap_cxl_bytes_ = 0;

  EpochStats last_epoch_{};
  uint64_t   epoch_count_ = 0;

  // Lifetime sums across all epochs (for end-of-simulation reporting).
  uint64_t total_ddr_bytes_ = 0;
  uint64_t total_cxl_bytes_ = 0;

public:
  M5Monitor() = default;

  // Called at each migration epoch boundary by M5Manager.
  //
  // cur_ddr_bytes: cumulative bytes_returned summed over all DDR channels
  // cur_cxl_bytes: cumulative bytes_returned summed over all CXL channels
  // ddr_pages    : DDR-resident page count from placement table
  // cxl_pages    : CXL-resident page count from placement table
  //
  // Returns the completed interval's stats and stores them in last_epoch_.
  EpochStats reset_epoch(uint64_t cur_ddr_bytes, uint64_t cur_cxl_bytes,
                         uint64_t ddr_pages,     uint64_t cxl_pages);

  const EpochStats& last_epoch_stats()    const { return last_epoch_; }
  uint64_t          epoch_count()         const { return epoch_count_; }
  uint64_t          total_ddr_read_bytes()const { return total_ddr_bytes_; }
  uint64_t          total_cxl_read_bytes()const { return total_cxl_bytes_; }
};

} // namespace m5

#endif // M5_MONITOR_H
