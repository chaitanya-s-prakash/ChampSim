#include "m5_monitor.h"

namespace m5
{

EpochStats M5Monitor::reset_epoch(uint64_t cur_ddr_bytes, uint64_t cur_cxl_bytes,
                                   uint64_t ddr_pages,     uint64_t cxl_pages)
{
  // Compute per-epoch deltas from cumulative counters.
  uint64_t interval_ddr = cur_ddr_bytes - snap_ddr_bytes_;
  uint64_t interval_cxl = cur_cxl_bytes - snap_cxl_bytes_;

  snap_ddr_bytes_ = cur_ddr_bytes;
  snap_cxl_bytes_ = cur_cxl_bytes;

  // Bandwidth density: bytes served per page resident in that tier.
  // A higher ratio means CXL pages are receiving more traffic per
  // unit of capacity than DDR pages — the signal to trigger migration.
  double ddr_density = (ddr_pages > 0)
                           ? static_cast<double>(interval_ddr) / static_cast<double>(ddr_pages)
                           : 0.0;
  double cxl_density = (cxl_pages > 0)
                           ? static_cast<double>(interval_cxl) / static_cast<double>(cxl_pages)
                           : 0.0;
  // Avoid divide-by-zero when DDR has no traffic yet.
  double ratio = (ddr_density > 0.0) ? cxl_density / ddr_density : 0.0;

  last_epoch_ = EpochStats{interval_ddr, interval_cxl, ddr_pages, cxl_pages,
                           ddr_density,  cxl_density,  ratio,     epoch_count_};

  total_ddr_bytes_ += interval_ddr;
  total_cxl_bytes_ += interval_cxl;
  ++epoch_count_;

  return last_epoch_;
}

} // namespace m5
