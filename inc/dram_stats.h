#ifndef DRAM_STATS_H
#define DRAM_STATS_H

#include <cstdint>
#include <string>

struct dram_stats {
  std::string name{};
  long dbus_cycle_congested{};
  uint64_t dbus_count_congested = 0;
  uint64_t cycles_elapsed = 0;
  uint64_t refresh_cycles = 0;
  uint64_t read_requests = 0;
  uint64_t write_requests = 0;
  uint64_t bytes_returned = 0;
  uint64_t bytes_transferred = 0;
  double theoretical_max_bytes = 0.0;
  uint64_t demand_requests = 0;
  uint64_t demand_tier_accesses = 0;
  uint64_t total_demand_latency_cycles = 0;
  uint64_t rq_capacity = 0;
  uint64_t wq_capacity = 0;
  bool is_secondary = false;
  uint64_t total_rq_queue_occupancy = 0;
  uint64_t total_wq_queue_occupancy = 0;
  uint64_t peak_rq_queue_occupancy = 0;
  uint64_t peak_wq_queue_occupancy = 0;
  uint64_t peak_total_queue_occupancy = 0;
  unsigned WQ_ROW_BUFFER_HIT = 0, WQ_ROW_BUFFER_MISS = 0, RQ_ROW_BUFFER_HIT = 0, RQ_ROW_BUFFER_MISS = 0, WQ_FULL = 0;
};

dram_stats operator-(dram_stats lhs, dram_stats rhs);

#endif
