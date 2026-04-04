#include <algorithm>
#include "dram_stats.h"

dram_stats operator-(dram_stats lhs, dram_stats rhs)
{
  lhs.dbus_cycle_congested -= rhs.dbus_cycle_congested;
  lhs.dbus_count_congested -= rhs.dbus_count_congested;
  lhs.cycles_elapsed -= rhs.cycles_elapsed;
  lhs.read_requests -= rhs.read_requests;
  lhs.write_requests -= rhs.write_requests;
  lhs.bytes_returned -= rhs.bytes_returned;
  lhs.bytes_transferred -= rhs.bytes_transferred;
  lhs.theoretical_max_bytes -= rhs.theoretical_max_bytes;
  lhs.demand_requests -= rhs.demand_requests;
  lhs.demand_tier_accesses -= rhs.demand_tier_accesses;
  lhs.total_demand_latency_cycles -= rhs.total_demand_latency_cycles;
  lhs.rq_capacity -= rhs.rq_capacity;
  lhs.wq_capacity -= rhs.wq_capacity;
  lhs.total_rq_queue_occupancy -= rhs.total_rq_queue_occupancy;
  lhs.total_wq_queue_occupancy -= rhs.total_wq_queue_occupancy;
  lhs.peak_rq_queue_occupancy = std::max(lhs.peak_rq_queue_occupancy, rhs.peak_rq_queue_occupancy);
  lhs.peak_wq_queue_occupancy = std::max(lhs.peak_wq_queue_occupancy, rhs.peak_wq_queue_occupancy);
  lhs.peak_total_queue_occupancy = std::max(lhs.peak_total_queue_occupancy, rhs.peak_total_queue_occupancy);
  lhs.WQ_ROW_BUFFER_HIT -= rhs.WQ_ROW_BUFFER_HIT;
  lhs.WQ_ROW_BUFFER_MISS -= rhs.WQ_ROW_BUFFER_MISS;
  lhs.RQ_ROW_BUFFER_HIT -= rhs.RQ_ROW_BUFFER_HIT;
  lhs.RQ_ROW_BUFFER_MISS -= rhs.RQ_ROW_BUFFER_MISS;
  lhs.WQ_FULL -= rhs.WQ_FULL;
  return lhs;
}
