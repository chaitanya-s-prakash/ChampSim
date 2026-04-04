#ifndef STATS_UTILS_H
#define STATS_UTILS_H

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "cache.h"
#include "dram_controller.h"

namespace champsim
{
inline uint64_t demand_accesses(const CACHE::stats_type& stats)
{
  uint64_t accesses = 0;
  for (const auto type : {access_type::LOAD, access_type::RFO, access_type::WRITE}) {
    for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu) {
      accesses += stats.hits.value_or(std::pair{type, cpu}, uint64_t{});
      accesses += stats.misses.value_or(std::pair{type, cpu}, uint64_t{});
    }
  }
  return accesses;
}

inline uint64_t demand_misses(const CACHE::stats_type& stats)
{
  uint64_t misses = 0;
  for (const auto type : {access_type::LOAD, access_type::RFO, access_type::WRITE}) {
    for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu) {
      misses += stats.misses.value_or(std::pair{type, cpu}, uint64_t{});
    }
  }
  return misses;
}

inline const CACHE::stats_type* find_cache_stats(const std::vector<CACHE::stats_type>& cache_stats, std::string_view name)
{
  auto it = std::find_if(std::begin(cache_stats), std::end(cache_stats), [name](const auto& stat) {
    return stat.name == name || (stat.name.size() >= name.size() && stat.name.compare(stat.name.size() - name.size(), name.size(), name) == 0);
  });
  return it != std::end(cache_stats) ? &*it : nullptr;
}

inline double average_offchip_demand_latency(const std::vector<DRAM_CHANNEL::stats_type>& dram_stats)
{
  uint64_t total_demand_requests = 0;
  uint64_t total_demand_latency_cycles = 0;
  for (const auto& stat : dram_stats) {
    total_demand_requests += stat.demand_requests;
    total_demand_latency_cycles += stat.total_demand_latency_cycles;
  }
  if (total_demand_requests == 0)
    return 0.0;
  return static_cast<double>(total_demand_latency_cycles) / static_cast<double>(total_demand_requests);
}

inline std::optional<double> hierarchy_amat_estimate(const std::vector<CACHE::stats_type>& cache_stats,
                                                     const std::vector<DRAM_CHANNEL::stats_type>& dram_stats)
{
  auto* l1d = find_cache_stats(cache_stats, "L1D");
  auto* l2c = find_cache_stats(cache_stats, "L2C");
  auto* llc = find_cache_stats(cache_stats, "LLC");

  if (l1d == nullptr || l2c == nullptr || llc == nullptr)
    return std::nullopt;

  auto l1d_accesses = demand_accesses(*l1d);
  auto l2c_accesses = demand_accesses(*l2c);
  auto llc_accesses = demand_accesses(*llc);

  if (l1d_accesses == 0 || l2c_accesses == 0 || llc_accesses == 0)
    return std::nullopt;

  auto l1d_miss_rate = static_cast<double>(demand_misses(*l1d)) / static_cast<double>(l1d_accesses);
  auto l2c_miss_rate = static_cast<double>(demand_misses(*l2c)) / static_cast<double>(l2c_accesses);
  auto llc_miss_rate = static_cast<double>(demand_misses(*llc)) / static_cast<double>(llc_accesses);
  auto memory_latency = average_offchip_demand_latency(dram_stats);

  return static_cast<double>(l1d->hit_latency_cycles)
         + l1d_miss_rate * (static_cast<double>(l2c->hit_latency_cycles)
                            + l2c_miss_rate * (static_cast<double>(llc->hit_latency_cycles) + llc_miss_rate * memory_latency));
}
} // namespace champsim

#endif
