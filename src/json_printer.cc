/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <optional>
#include <utility>
#include <nlohmann/json.hpp>

#include "stats_printer.h"
#include "stats_utils.h"

namespace
{
std::optional<double> safe_divide(double numerator, double denominator)
{
  if (denominator <= 0.0) {
    return std::nullopt;
  }
  return numerator / denominator;
}
} // namespace

void to_json(nlohmann::json& j, const O3_CPU::stats_type& stats)
{
  constexpr std::array types{branch_type::BRANCH_DIRECT_JUMP, branch_type::BRANCH_INDIRECT,      branch_type::BRANCH_CONDITIONAL,
                             branch_type::BRANCH_DIRECT_CALL, branch_type::BRANCH_INDIRECT_CALL, branch_type::BRANCH_RETURN};

  auto total_mispredictions = std::ceil(
      std::accumulate(std::begin(types), std::end(types), 0LL, [btm = stats.branch_type_misses](auto acc, auto next) { return acc + btm.value_or(next, 0); }));

  std::map<std::string, std::size_t> mpki{};
  for (auto type : types) {
    mpki.emplace(branch_type_names.at(champsim::to_underlying(type)), stats.branch_type_misses.value_or(type, 0));
  }

  j = nlohmann::json{{"instructions", stats.instrs()},
                     {"cycles", stats.cycles()},
                     {"Avg ROB occupancy at mispredict", std::ceil(stats.total_rob_occupancy_at_branch_mispredict) / std::ceil(total_mispredictions)},
                     {"mispredict", mpki}};
}

void to_json(nlohmann::json& j, const CACHE::stats_type& stats)
{
  using hits_value_type = typename decltype(stats.hits)::value_type;
  using misses_value_type = typename decltype(stats.misses)::value_type;
  using mshr_merge_value_type = typename decltype(stats.mshr_merge)::value_type;
  using mshr_return_value_type = typename decltype(stats.mshr_return)::value_type;

  std::map<std::string, nlohmann::json> statsmap;
  statsmap.emplace("prefetch requested", stats.pf_requested);
  statsmap.emplace("prefetch issued", stats.pf_issued);
  statsmap.emplace("useful prefetch", stats.pf_useful);
  statsmap.emplace("useless prefetch", stats.pf_useless);
  statsmap.emplace("hit latency", stats.hit_latency_cycles);

  uint64_t total_downstream_demands = stats.mshr_return.total();
  for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu)
    total_downstream_demands -= stats.mshr_return.value_or(std::pair{access_type::PREFETCH, cpu}, mshr_return_value_type{});

  statsmap.emplace("miss latency", std::ceil(stats.total_miss_latency_cycles) / std::ceil(total_downstream_demands));
  for (const auto type : {access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION}) {
    std::vector<hits_value_type> hits;
    std::vector<misses_value_type> misses;
    std::vector<mshr_merge_value_type> mshr_merges;

    for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu) {
      hits.push_back(stats.hits.value_or(std::pair{type, cpu}, hits_value_type{}));
      misses.push_back(stats.misses.value_or(std::pair{type, cpu}, misses_value_type{}));
      mshr_merges.push_back(stats.mshr_merge.value_or(std::pair{type, cpu}, mshr_merge_value_type{}));
    }

    statsmap.emplace(access_type_names.at(champsim::to_underlying(type)), nlohmann::json{{"hit", hits}, {"miss", misses}, {"mshr_merge", mshr_merges}});
  }

  j = statsmap;
}

void to_json(nlohmann::json& j, const DRAM_CHANNEL::stats_type stats)
{
  auto average_rq_occupancy = safe_divide(static_cast<double>(stats.total_rq_queue_occupancy), static_cast<double>(stats.cycles_elapsed));
  auto average_wq_occupancy = safe_divide(static_cast<double>(stats.total_wq_queue_occupancy), static_cast<double>(stats.cycles_elapsed));
  auto average_total_occupancy =
      safe_divide(static_cast<double>(stats.total_rq_queue_occupancy + stats.total_wq_queue_occupancy), static_cast<double>(stats.cycles_elapsed));
  auto average_rq_occupancy_ratio = safe_divide(static_cast<double>(stats.total_rq_queue_occupancy),
                                                static_cast<double>(stats.cycles_elapsed) * static_cast<double>(stats.rq_capacity));
  auto average_wq_occupancy_ratio = safe_divide(static_cast<double>(stats.total_wq_queue_occupancy),
                                                static_cast<double>(stats.cycles_elapsed) * static_cast<double>(stats.wq_capacity));
  auto average_total_occupancy_ratio =
      safe_divide(static_cast<double>(stats.total_rq_queue_occupancy + stats.total_wq_queue_occupancy),
                  static_cast<double>(stats.cycles_elapsed) * static_cast<double>(stats.rq_capacity + stats.wq_capacity));
  auto peak_rq_occupancy_ratio = safe_divide(static_cast<double>(stats.peak_rq_queue_occupancy), static_cast<double>(stats.rq_capacity));
  auto peak_wq_occupancy_ratio = safe_divide(static_cast<double>(stats.peak_wq_queue_occupancy), static_cast<double>(stats.wq_capacity));
  auto peak_total_occupancy_ratio =
      safe_divide(static_cast<double>(stats.peak_total_queue_occupancy), static_cast<double>(stats.rq_capacity + stats.wq_capacity));
  auto bandwidth_utilization = safe_divide(static_cast<double>(stats.bytes_transferred), stats.theoretical_max_bytes);

  j = nlohmann::json{{"READ_REQUESTS", stats.read_requests},
                     {"WRITE_REQUESTS", stats.write_requests},
                     {"BYTES_RETURNED", stats.bytes_returned},
                     {"BYTES_TRANSFERRED", stats.bytes_transferred},
                     {"BANDWIDTH_UTILIZATION", bandwidth_utilization.value_or(0.0)},
                     {"DEMAND_REQUESTS", stats.demand_requests},
                     {"DEMAND_TIER_ACCESSES", stats.demand_tier_accesses},
                     {"AVERAGE DEMAND ACCESS LATENCY", (std::ceil(stats.total_demand_latency_cycles) / std::ceil(stats.demand_requests))},
                     {"AVERAGE_RQ_OCCUPANCY", average_rq_occupancy.value_or(0.0)},
                     {"AVERAGE_WQ_OCCUPANCY", average_wq_occupancy.value_or(0.0)},
                     {"AVERAGE_TOTAL_QUEUE_OCCUPANCY", average_total_occupancy.value_or(0.0)},
                     {"AVERAGE_RQ_OCCUPANCY_RATIO", average_rq_occupancy_ratio.value_or(0.0)},
                     {"AVERAGE_WQ_OCCUPANCY_RATIO", average_wq_occupancy_ratio.value_or(0.0)},
                     {"AVERAGE_TOTAL_QUEUE_OCCUPANCY_RATIO", average_total_occupancy_ratio.value_or(0.0)},
                     {"PEAK_RQ_OCCUPANCY", stats.peak_rq_queue_occupancy},
                     {"PEAK_WQ_OCCUPANCY", stats.peak_wq_queue_occupancy},
                     {"PEAK_TOTAL_QUEUE_OCCUPANCY", stats.peak_total_queue_occupancy},
                     {"PEAK_RQ_OCCUPANCY_RATIO", peak_rq_occupancy_ratio.value_or(0.0)},
                     {"PEAK_WQ_OCCUPANCY_RATIO", peak_wq_occupancy_ratio.value_or(0.0)},
                     {"PEAK_TOTAL_QUEUE_OCCUPANCY_RATIO", peak_total_occupancy_ratio.value_or(0.0)},
                     {"RQ ROW_BUFFER_HIT", stats.RQ_ROW_BUFFER_HIT},
                     {"RQ ROW_BUFFER_MISS", stats.RQ_ROW_BUFFER_MISS},
                     {"WQ ROW_BUFFER_HIT", stats.WQ_ROW_BUFFER_HIT},
                     {"WQ ROW_BUFFER_MISS", stats.WQ_ROW_BUFFER_MISS},
                     {"AVG DBUS CONGESTED CYCLE", (std::ceil(stats.dbus_cycle_congested) / std::ceil(stats.dbus_count_congested))},
                     {"REFRESHES ISSUED", stats.refresh_cycles}};
}

void to_json(nlohmann::json& j, const vmem_stats& stats)
{
  j = nlohmann::json{{"DDR_CAPACITY_PAGES", stats.ddr_capacity_pages},
                     {"CXL_CAPACITY_PAGES", stats.cxl_capacity_pages},
                     {"DDR_PAGE_ALLOCATIONS", stats.ddr_page_allocations},
                     {"CXL_PAGE_ALLOCATIONS", stats.cxl_page_allocations},
                     {"CURRENT_DDR_PAGES", stats.current_ddr_pages},
                     {"CURRENT_CXL_PAGES", stats.current_cxl_pages},
                     {"PEAK_DDR_PAGES", stats.peak_ddr_pages},
                     {"PEAK_CXL_PAGES", stats.peak_cxl_pages}};
}

namespace champsim
{
void to_json(nlohmann::json& j, const champsim::phase_stats stats)
{
  auto tier_summary = [](const std::vector<DRAM_CHANNEL::stats_type>& dram) {
    uint64_t ddr_accesses = 0, total_accesses = 0;
    uint64_t ddr_lat = 0, ddr_reqs = 0, cxl_lat = 0, cxl_reqs = 0;
    for (const auto& s : dram) {
      total_accesses += s.demand_tier_accesses;
      if (!s.is_secondary) {
        ddr_accesses += s.demand_tier_accesses;
        ddr_lat += s.total_demand_latency_cycles;
        ddr_reqs += s.demand_requests;
      } else {
        cxl_lat += s.total_demand_latency_cycles;
        cxl_reqs += s.demand_requests;
      }
    }
    double hit_rate = total_accesses > 0 ? static_cast<double>(ddr_accesses) / static_cast<double>(total_accesses) : 0.0;
    double avg_ddr_lat = ddr_reqs > 0 ? static_cast<double>(ddr_lat) / static_cast<double>(ddr_reqs) : 0.0;
    double avg_cxl_lat = cxl_reqs > 0 ? static_cast<double>(cxl_lat) / static_cast<double>(cxl_reqs) : 0.0;
    return std::tuple{ddr_accesses, hit_rate, avg_ddr_lat, avg_cxl_lat};
  };

  auto [roi_ddr_accesses, roi_ddr_rate, roi_avg_ddr_lat, roi_avg_cxl_lat] = tier_summary(stats.roi_dram_stats);
  auto [sim_ddr_accesses, sim_ddr_rate, sim_avg_ddr_lat, sim_avg_cxl_lat] = tier_summary(stats.sim_dram_stats);

  std::map<std::string, nlohmann::json> roi_stats;
  roi_stats.emplace("cores", stats.roi_cpu_stats);
  roi_stats.emplace("DRAM", stats.roi_dram_stats);
  roi_stats.emplace("DDR_TIER_ACCESSES", roi_ddr_accesses);
  roi_stats.emplace("DDR_TIER_ACCESS_RATE", roi_ddr_rate);
  roi_stats.emplace("AVERAGE_OFFCHIP_DEMAND_ACCESS_LATENCY", average_offchip_demand_latency(stats.roi_dram_stats));
  roi_stats.emplace("AVERAGE_DDR_DEMAND_ACCESS_LATENCY", roi_avg_ddr_lat);
  roi_stats.emplace("AVERAGE_CXL_DEMAND_ACCESS_LATENCY", roi_avg_cxl_lat);
  roi_stats.emplace("HIERARCHY_AMAT_ESTIMATE",
                    hierarchy_amat_estimate(stats.roi_cache_stats, stats.roi_dram_stats).value_or(0.0));
  roi_stats.emplace("Virtual Memory", stats.roi_vmem_stats);
  for (auto x : stats.roi_cache_stats) {
    roi_stats.emplace(x.name, x);
  }

  std::map<std::string, nlohmann::json> sim_stats;
  sim_stats.emplace("cores", stats.sim_cpu_stats);
  sim_stats.emplace("DRAM", stats.sim_dram_stats);
  sim_stats.emplace("DDR_TIER_ACCESSES", sim_ddr_accesses);
  sim_stats.emplace("DDR_TIER_ACCESS_RATE", sim_ddr_rate);
  sim_stats.emplace("AVERAGE_OFFCHIP_DEMAND_ACCESS_LATENCY", average_offchip_demand_latency(stats.sim_dram_stats));
  sim_stats.emplace("AVERAGE_DDR_DEMAND_ACCESS_LATENCY", sim_avg_ddr_lat);
  sim_stats.emplace("AVERAGE_CXL_DEMAND_ACCESS_LATENCY", sim_avg_cxl_lat);
  sim_stats.emplace("HIERARCHY_AMAT_ESTIMATE",
                    hierarchy_amat_estimate(stats.sim_cache_stats, stats.sim_dram_stats).value_or(0.0));
  sim_stats.emplace("Virtual Memory", stats.sim_vmem_stats);
  for (auto x : stats.sim_cache_stats) {
    sim_stats.emplace(x.name, x);
  }

  std::map<std::string, nlohmann::json> statsmap{{"name", stats.name}, {"traces", stats.trace_names}};
  statsmap.emplace("roi", roi_stats);
  statsmap.emplace("sim", sim_stats);
  j = statsmap;
}
} // namespace champsim

void champsim::json_printer::print(std::vector<phase_stats>& stats) { stream << nlohmann::json::array_t{std::begin(stats), std::end(stats)}; }
