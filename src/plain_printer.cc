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

#include <cmath>
#include <numeric>
#include <ratio>
#include <string_view> // for string_view
#include <utility>
#include <vector>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <fmt/ostream.h>

#include "stats_printer.h"
#include "stats_utils.h"

namespace
{
template <typename N, typename D>
auto print_ratio(N num, D denom)
{
  if (denom > 0) {
    return fmt::format("{:.4g}", std::ceil(num) / std::ceil(denom));
  }
  return std::string{"-"};
}

auto print_division(double num, double denom)
{
  if (denom > 0.0) {
    return fmt::format("{:.4g}", num / denom);
  }
  return std::string{"-"};
}
} // namespace

using champsim::average_offchip_demand_latency;
using champsim::hierarchy_amat_estimate;

std::vector<std::string> champsim::plain_printer::format(O3_CPU::stats_type stats)
{
  constexpr std::array types{branch_type::BRANCH_DIRECT_JUMP, branch_type::BRANCH_INDIRECT,      branch_type::BRANCH_CONDITIONAL,
                             branch_type::BRANCH_DIRECT_CALL, branch_type::BRANCH_INDIRECT_CALL, branch_type::BRANCH_RETURN};
  auto total_branch = std::ceil(
      std::accumulate(std::begin(types), std::end(types), 0LL, [tbt = stats.total_branch_types](auto acc, auto next) { return acc + tbt.value_or(next, 0); }));
  auto total_mispredictions = std::ceil(
      std::accumulate(std::begin(types), std::end(types), 0LL, [btm = stats.branch_type_misses](auto acc, auto next) { return acc + btm.value_or(next, 0); }));

  std::vector<std::string> lines{};
  lines.push_back(fmt::format("{} cumulative IPC: {} instructions: {} cycles: {}", stats.name, ::print_ratio(stats.instrs(), stats.cycles()), stats.instrs(),
                              stats.cycles()));

  lines.push_back(fmt::format("{} Branch Prediction Accuracy: {}% MPKI: {} Average ROB Occupancy at Mispredict: {}", stats.name,
                              ::print_ratio(100 * (total_branch - total_mispredictions), total_branch),
                              ::print_ratio(std::kilo::num * total_mispredictions, stats.instrs()),
                              ::print_ratio(stats.total_rob_occupancy_at_branch_mispredict, total_mispredictions)));

  lines.emplace_back("Branch type MPKI");
  for (auto idx : types) {
    lines.push_back(fmt::format("{}: {}", branch_type_names.at(champsim::to_underlying(idx)),
                                ::print_ratio(std::kilo::num * stats.branch_type_misses.value_or(idx, 0), stats.instrs())));
  }

  return lines;
}

std::vector<std::string> champsim::plain_printer::format(CACHE::stats_type stats)
{
  using hits_value_type = typename decltype(stats.hits)::value_type;
  using misses_value_type = typename decltype(stats.misses)::value_type;
  using mshr_merge_value_type = typename decltype(stats.mshr_merge)::value_type;
  using mshr_return_value_type = typename decltype(stats.mshr_return)::value_type;

  std::vector<std::size_t> cpus;

  // build a vector of all existing cpus
  auto stat_keys = {stats.hits.get_keys(), stats.misses.get_keys(), stats.mshr_merge.get_keys(), stats.mshr_return.get_keys()};
  for (auto keys : stat_keys) {
    std::transform(std::begin(keys), std::end(keys), std::back_inserter(cpus), [](auto val) { return val.second; });
  }
  std::sort(std::begin(cpus), std::end(cpus));
  auto uniq_end = std::unique(std::begin(cpus), std::end(cpus));
  cpus.erase(uniq_end, std::end(cpus));

  for (const auto type : {access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION}) {
    for (auto cpu : cpus) {
      stats.hits.allocate(std::pair{type, cpu});
      stats.misses.allocate(std::pair{type, cpu});
      stats.mshr_merge.allocate(std::pair{type, cpu});
      stats.mshr_return.allocate(std::pair{type, cpu});
    }
  }

  std::vector<std::string> lines{};
  for (auto cpu : cpus) {
    hits_value_type total_hits = 0;
    misses_value_type total_misses = 0;
    mshr_merge_value_type total_mshr_merge = 0;
    mshr_return_value_type total_mshr_return = 0;
    for (const auto type : {access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION}) {
      total_hits += stats.hits.value_or(std::pair{type, cpu}, hits_value_type{});
      total_misses += stats.misses.value_or(std::pair{type, cpu}, misses_value_type{});
      total_mshr_merge += stats.mshr_merge.value_or(std::pair{type, cpu}, mshr_merge_value_type{});
      total_mshr_return += stats.mshr_return.value_or(std::pair{type, cpu}, mshr_merge_value_type{});
    }

    fmt::format_string<std::string_view, std::string_view, int, int, int> hitmiss_fmtstr{
        "cpu{}->{} {:<12s} ACCESS: {:10d} HIT: {:10d} MISS: {:10d} MSHR_MERGE: {:10d}"};
    lines.push_back(fmt::format(hitmiss_fmtstr, cpu, stats.name, "TOTAL", total_hits + total_misses, total_hits, total_misses, total_mshr_merge));
    for (const auto type : {access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION}) {
      lines.push_back(
          fmt::format(hitmiss_fmtstr, cpu, stats.name, access_type_names.at(champsim::to_underlying(type)),
                      stats.hits.value_or(std::pair{type, cpu}, hits_value_type{}) + stats.misses.value_or(std::pair{type, cpu}, misses_value_type{}),
                      stats.hits.value_or(std::pair{type, cpu}, hits_value_type{}), stats.misses.value_or(std::pair{type, cpu}, misses_value_type{}),
                      stats.mshr_merge.value_or(std::pair{type, cpu}, mshr_merge_value_type{})));
    }

  lines.push_back(fmt::format("cpu{}->{} PREFETCH REQUESTED: {:10} ISSUED: {:10} USEFUL: {:10} USELESS: {:10}", cpu, stats.name, stats.pf_requested,
                                stats.pf_issued, stats.pf_useful, stats.pf_useless));

  uint64_t total_downstream_demands = total_mshr_return - stats.mshr_return.value_or(std::pair{access_type::PREFETCH, cpu}, mshr_return_value_type{});
  lines.push_back(fmt::format("cpu{}->{} HIT LATENCY: {} cycles", cpu, stats.name, stats.hit_latency_cycles));
  lines.push_back(
        fmt::format("cpu{}->{} AVERAGE MISS LATENCY: {} cycles", cpu, stats.name, ::print_ratio(stats.total_miss_latency_cycles, total_downstream_demands)));
  }

  return lines;
}

std::vector<std::string> champsim::plain_printer::format(DRAM_CHANNEL::stats_type stats)
{
  auto average_rq_occupancy = print_division(static_cast<double>(stats.total_rq_queue_occupancy), static_cast<double>(stats.cycles_elapsed));
  auto average_wq_occupancy = print_division(static_cast<double>(stats.total_wq_queue_occupancy), static_cast<double>(stats.cycles_elapsed));
  auto average_total_occupancy =
      print_division(static_cast<double>(stats.total_rq_queue_occupancy + stats.total_wq_queue_occupancy), static_cast<double>(stats.cycles_elapsed));
  auto average_rq_occupancy_ratio =
      print_division(static_cast<double>(stats.total_rq_queue_occupancy), static_cast<double>(stats.cycles_elapsed) * static_cast<double>(stats.rq_capacity));
  auto average_wq_occupancy_ratio =
      print_division(static_cast<double>(stats.total_wq_queue_occupancy), static_cast<double>(stats.cycles_elapsed) * static_cast<double>(stats.wq_capacity));
  auto average_total_occupancy_ratio = print_division(static_cast<double>(stats.total_rq_queue_occupancy + stats.total_wq_queue_occupancy),
                                                      static_cast<double>(stats.cycles_elapsed) * static_cast<double>(stats.rq_capacity + stats.wq_capacity));
  auto peak_rq_occupancy_ratio = print_division(static_cast<double>(stats.peak_rq_queue_occupancy), static_cast<double>(stats.rq_capacity));
  auto peak_wq_occupancy_ratio = print_division(static_cast<double>(stats.peak_wq_queue_occupancy), static_cast<double>(stats.wq_capacity));
  auto peak_total_occupancy_ratio =
      print_division(static_cast<double>(stats.peak_total_queue_occupancy), static_cast<double>(stats.rq_capacity + stats.wq_capacity));
  auto bandwidth_utilization = print_division(static_cast<double>(stats.bytes_transferred), stats.theoretical_max_bytes);

  std::vector<std::string> lines{};
  lines.push_back(fmt::format("{} READ_REQUESTS: {:10}", stats.name, stats.read_requests));
  lines.push_back(fmt::format("  WRITE_REQUESTS: {:10}", stats.write_requests));
  lines.push_back(fmt::format("  BYTES_RETURNED: {:10}", stats.bytes_returned));
  lines.push_back(fmt::format("  BYTES_TRANSFERRED: {:10}", stats.bytes_transferred));
  lines.push_back(fmt::format("  BANDWIDTH UTILIZATION: {}", bandwidth_utilization));
  lines.push_back(fmt::format("  DEMAND_REQUESTS: {:10}", stats.demand_requests));
  lines.push_back(fmt::format("  DEMAND_TIER_ACCESSES: {:10}", stats.demand_tier_accesses));
  lines.push_back(fmt::format("  AVERAGE DEMAND ACCESS LATENCY: {} cycles", ::print_ratio(stats.total_demand_latency_cycles, stats.demand_requests)));
  lines.push_back(fmt::format("  AVERAGE RQ OCCUPANCY: {}", average_rq_occupancy));
  lines.push_back(fmt::format("  AVERAGE WQ OCCUPANCY: {}", average_wq_occupancy));
  lines.push_back(fmt::format("  AVERAGE TOTAL QUEUE OCCUPANCY: {}", average_total_occupancy));
  lines.push_back(fmt::format("  AVERAGE RQ OCCUPANCY RATIO: {}", average_rq_occupancy_ratio));
  lines.push_back(fmt::format("  AVERAGE WQ OCCUPANCY RATIO: {}", average_wq_occupancy_ratio));
  lines.push_back(fmt::format("  AVERAGE TOTAL QUEUE OCCUPANCY RATIO: {}", average_total_occupancy_ratio));
  lines.push_back(fmt::format("  PEAK RQ OCCUPANCY: {:10}", stats.peak_rq_queue_occupancy));
  lines.push_back(fmt::format("  PEAK WQ OCCUPANCY: {:10}", stats.peak_wq_queue_occupancy));
  lines.push_back(fmt::format("  PEAK TOTAL QUEUE OCCUPANCY: {:10}", stats.peak_total_queue_occupancy));
  lines.push_back(fmt::format("  PEAK RQ OCCUPANCY RATIO: {}", peak_rq_occupancy_ratio));
  lines.push_back(fmt::format("  PEAK WQ OCCUPANCY RATIO: {}", peak_wq_occupancy_ratio));
  lines.push_back(fmt::format("  PEAK TOTAL QUEUE OCCUPANCY RATIO: {}", peak_total_occupancy_ratio));
  lines.push_back(fmt::format("{} RQ ROW_BUFFER_HIT: {:10}", stats.name, stats.RQ_ROW_BUFFER_HIT));
  lines.push_back(fmt::format("  ROW_BUFFER_MISS: {:10}", stats.RQ_ROW_BUFFER_MISS));
  lines.push_back(fmt::format("  AVG DBUS CONGESTED CYCLE: {}", ::print_ratio(stats.dbus_cycle_congested, stats.dbus_count_congested)));
  lines.push_back(fmt::format("{} WQ ROW_BUFFER_HIT: {:10}", stats.name, stats.WQ_ROW_BUFFER_HIT));
  lines.push_back(fmt::format("  ROW_BUFFER_MISS: {:10}", stats.WQ_ROW_BUFFER_MISS));
  lines.push_back(fmt::format("  FULL: {:10}", stats.WQ_FULL));

  if (stats.refresh_cycles > 0)
    lines.push_back(fmt::format("{} REFRESHES ISSUED: {:10}", stats.name, stats.refresh_cycles));
  else
    lines.push_back(fmt::format("{} REFRESHES ISSUED: -", stats.name));

  return lines;
}

std::vector<std::string> champsim::plain_printer::format(vmem_stats stats)
{
  std::vector<std::string> lines{};
  lines.push_back(fmt::format("{} DDR_CAPACITY_PAGES: {:10}", stats.name, stats.ddr_capacity_pages));
  lines.push_back(fmt::format("  CXL_CAPACITY_PAGES: {:10}", stats.cxl_capacity_pages));
  lines.push_back(fmt::format("{} DDR_PAGE_ALLOCATIONS: {:10}", stats.name, stats.ddr_page_allocations));
  lines.push_back(fmt::format("  CXL_PAGE_ALLOCATIONS: {:10}", stats.cxl_page_allocations));
  lines.push_back(fmt::format("{} CURRENT_DDR_PAGES: {:10}", stats.name, stats.current_ddr_pages));
  lines.push_back(fmt::format("  CURRENT_CXL_PAGES: {:10}", stats.current_cxl_pages));
  lines.push_back(fmt::format("{} PEAK_DDR_PAGES: {:10}", stats.name, stats.peak_ddr_pages));
  lines.push_back(fmt::format("  PEAK_CXL_PAGES: {:10}", stats.peak_cxl_pages));

  return lines;
}

void champsim::plain_printer::print(champsim::phase_stats& stats)
{
  auto lines = format(stats);
  std::copy(std::begin(lines), std::end(lines), std::ostream_iterator<std::string>(stream, "\n"));
}

std::vector<std::string> champsim::plain_printer::format(champsim::phase_stats& stats)
{
  std::vector<std::string> lines{};
  lines.push_back(fmt::format("=== {} ===", stats.name));

  int i = 0;
  for (auto tn : stats.trace_names) {
    lines.push_back(fmt::format("CPU {} runs {}", i++, tn));
  }

  if (NUM_CPUS > 1) {
    lines.emplace_back("");
    lines.emplace_back("Total Simulation Statistics (not including warmup)");

    for (const auto& stat : stats.sim_cpu_stats) {
      auto sublines = format(stat);
      lines.emplace_back("");
      std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
      lines.emplace_back("");
    }

    for (const auto& stat : stats.sim_cache_stats) {
      auto sublines = format(stat);
      std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
    }

    lines.emplace_back("");
    lines.emplace_back("Virtual Memory Statistics");
    auto sim_vmem_lines = format(stats.sim_vmem_stats);
    std::move(std::begin(sim_vmem_lines), std::end(sim_vmem_lines), std::back_inserter(lines));
  }

  lines.emplace_back("");
  lines.emplace_back("Region of Interest Statistics");

  for (const auto& stat : stats.roi_cpu_stats) {
    auto sublines = format(stat);
    lines.emplace_back("");
    std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
    lines.emplace_back("");
  }

  for (const auto& stat : stats.roi_cache_stats) {
    auto sublines = format(stat);
    std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
  }

  lines.emplace_back("");
  lines.emplace_back("DRAM Statistics");
  for (const auto& stat : stats.roi_dram_stats) {
    auto sublines = format(stat);
    lines.emplace_back("");
    std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
  }

  uint64_t ddr_tier_accesses = 0;
  uint64_t total_tier_accesses = 0;
  uint64_t ddr_latency_cycles = 0;
  uint64_t ddr_demand_requests = 0;
  uint64_t cxl_latency_cycles = 0;
  uint64_t cxl_demand_requests = 0;
  for (const auto& stat : stats.roi_dram_stats) {
    total_tier_accesses += stat.demand_tier_accesses;
    if (!stat.is_secondary) {
      ddr_tier_accesses += stat.demand_tier_accesses;
      ddr_latency_cycles += stat.total_demand_latency_cycles;
      ddr_demand_requests += stat.demand_requests;
    } else {
      cxl_latency_cycles += stat.total_demand_latency_cycles;
      cxl_demand_requests += stat.demand_requests;
    }
  }
  lines.emplace_back("");
  lines.push_back(fmt::format("DDR TIER ACCESSES: {:10}", ddr_tier_accesses));
  lines.push_back(fmt::format("DDR TIER ACCESS RATE: {}", ::print_ratio(ddr_tier_accesses, total_tier_accesses)));
  auto total_offchip_demand_latency_cycles = ddr_latency_cycles + cxl_latency_cycles;
  auto total_offchip_demand_requests = ddr_demand_requests + cxl_demand_requests;
  lines.push_back(
      fmt::format("AVERAGE OFF-CHIP DEMAND ACCESS LATENCY: {} cycles", ::print_ratio(total_offchip_demand_latency_cycles, total_offchip_demand_requests)));
  if (ddr_demand_requests > 0)
    lines.push_back(fmt::format("AVERAGE DDR DEMAND ACCESS LATENCY: {} cycles", ::print_ratio(ddr_latency_cycles, ddr_demand_requests)));
  if (cxl_demand_requests > 0)
    lines.push_back(fmt::format("AVERAGE CXL DEMAND ACCESS LATENCY: {} cycles", ::print_ratio(cxl_latency_cycles, cxl_demand_requests)));

  // M5 Monitor: lifetime bandwidth density (computed from ROI stats).
  // density = bytes_returned / pages_resident.
  // CXL/DDR ratio > 1 means CXL holds hotter pages per unit capacity → migration is beneficial.
  {
    uint64_t ddr_bytes = 0, cxl_bytes = 0;
    for (const auto& s : stats.roi_dram_stats) {
      if (!s.is_secondary)
        ddr_bytes += s.bytes_returned;
      else
        cxl_bytes += s.bytes_returned;
    }
    uint64_t ddr_pages = stats.roi_vmem_stats.current_ddr_pages;
    uint64_t cxl_pages = stats.roi_vmem_stats.current_cxl_pages;
    double ddr_density = (ddr_pages > 0) ? static_cast<double>(ddr_bytes) / static_cast<double>(ddr_pages) : 0.0;
    double cxl_density = (cxl_pages > 0) ? static_cast<double>(cxl_bytes) / static_cast<double>(cxl_pages) : 0.0;
    double ratio       = (ddr_density > 0.0) ? cxl_density / ddr_density : 0.0;
    lines.emplace_back("");
    lines.emplace_back("M5 Monitor Bandwidth Density (lifetime ROI)");
    lines.push_back(fmt::format("  DDR READ BYTES: {:12}  CXL READ BYTES: {:12}", ddr_bytes, cxl_bytes));
    lines.push_back(fmt::format("  DDR PAGES:      {:12}  CXL PAGES:      {:12}", ddr_pages, cxl_pages));
    lines.push_back(fmt::format("  DDR BW DENSITY: {:.4f} bytes/page", ddr_density));
    lines.push_back(fmt::format("  CXL BW DENSITY: {:.4f} bytes/page", cxl_density));
    lines.push_back(fmt::format("  CXL/DDR DENSITY RATIO: {:.4f}  (>1 means migration is beneficial)", ratio));
  }

  if (auto amat = hierarchy_amat_estimate(stats.roi_cache_stats, stats.roi_dram_stats); amat.has_value()) {
    lines.push_back(fmt::format("HIERARCHY AMAT ESTIMATE: {:.4f} cycles", amat.value()));
  } else {
    lines.emplace_back("HIERARCHY AMAT ESTIMATE: - cycles");
  }

  lines.emplace_back("");
  lines.emplace_back("Virtual Memory Statistics");
  auto roi_vmem_lines = format(stats.roi_vmem_stats);
  std::move(std::begin(roi_vmem_lines), std::end(roi_vmem_lines), std::back_inserter(lines));

  return lines;
}

void champsim::plain_printer::print(std::vector<phase_stats>& stats)
{
  for (auto p : stats) {
    print(p);
  }
}
