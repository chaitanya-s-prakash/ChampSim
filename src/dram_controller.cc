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

#include "dram_controller.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <fmt/core.h>

#include "deadlock.h"
#include "instruction.h"
#include "util/bits.h" // for lg2, bitmask
#include "util/span.h"
#include "util/units.h"

namespace
{
champsim::address subtract_offset(champsim::address address, champsim::address offset)
{
  return champsim::address{address.to<uint64_t>() - offset.to<uint64_t>()};
}
} // namespace

DRAM_ADDRESS_MAPPING MEMORY_CONTROLLER::make_address_mapping(const memory_spec& spec)
{
  return DRAM_ADDRESS_MAPPING{spec.channel_width, static_cast<std::size_t>(BLOCK_SIZE / spec.channel_width.count()), spec.channels, spec.bankgroups, spec.banks, spec.columns,
                              spec.ranks, spec.rows};
}

champsim::data::bytes MEMORY_CONTROLLER::memory_size(const DRAM_ADDRESS_MAPPING& mapping)
{
  return champsim::data::bytes{(1ll << mapping.address_slicer.bit_size())};
}

MEMORY_CONTROLLER::MEMORY_CONTROLLER(std::vector<channel_type*>&& ul, memory_spec primary, std::optional<memory_spec> secondary)
    : champsim::operable(primary.mc_period), queues(std::move(ul)), primary_channel_width(primary.channel_width),
      secondary_channel_width(secondary.has_value() ? std::optional<champsim::data::bytes>{secondary->channel_width} : std::nullopt),
      primary_channel_count(primary.channels), primary_name(primary.name), secondary_name(secondary.has_value() ? std::optional<std::string>{secondary->name} : std::nullopt),
      primary_size_bytes(memory_size(make_address_mapping(primary))),
      secondary_size_bytes(secondary.has_value() ? memory_size(make_address_mapping(*secondary)) : champsim::data::bytes{}), address_mapping(make_address_mapping(primary)),
      secondary_address_mapping(secondary.has_value() ? std::optional<DRAM_ADDRESS_MAPPING>{make_address_mapping(*secondary)} : std::nullopt),
      data_bus_period(primary.dbus_period),
      // Placement table: initialized from the computed tier capacities.
      // primary_size_bytes and secondary_size_bytes are already initialized above
      // (members are constructed in declaration order).
      placement_table(
          static_cast<uint64_t>(primary_size_bytes.count()) / PAGE_SIZE,
          static_cast<uint64_t>(secondary_size_bytes.count()) / PAGE_SIZE,
          static_cast<uint64_t>(primary_size_bytes.count()))
{
  channels.reserve(primary.channels + (secondary.has_value() ? secondary->channels : 0));

  for (std::size_t i{0}; i < primary.channels; ++i) {
    channels.emplace_back(primary.dbus_period, primary.mc_period, primary.t_rp, primary.t_rcd, primary.t_cas, primary.t_ras, primary.refresh_period,
                          primary.refreshes_per_period, primary.channel_width, primary.rq_size, primary.wq_size, address_mapping,
                          primary.name + " Channel " + std::to_string(i));
  }

  if (secondary.has_value() && secondary_address_mapping.has_value()) {
    auto secondary_offset = champsim::lowest_address_for_size(primary_size_bytes);
    for (std::size_t i{0}; i < secondary->channels; ++i) {
      channels.emplace_back(secondary->dbus_period, secondary->mc_period, secondary->t_rp, secondary->t_rcd, secondary->t_cas, secondary->t_ras,
                            secondary->refresh_period, secondary->refreshes_per_period, secondary->channel_width, secondary->rq_size, secondary->wq_size,
                            *secondary_address_mapping, secondary->name + " Channel " + std::to_string(i), secondary_offset);
    }
  }
}

MEMORY_CONTROLLER::MEMORY_CONTROLLER(champsim::chrono::picoseconds dbus_period, champsim::chrono::picoseconds mc_period, std::size_t t_rp, std::size_t t_rcd,
                                     std::size_t t_cas, std::size_t t_ras, champsim::chrono::microseconds refresh_period, std::vector<channel_type*>&& ul,
                                     std::size_t rq_size, std::size_t wq_size, std::size_t chans, champsim::data::bytes chan_width, std::size_t rows,
                                     std::size_t columns, std::size_t ranks, std::size_t bankgroups, std::size_t banks, std::size_t refreshes_per_period)
    : MEMORY_CONTROLLER(std::move(ul), memory_spec{"DRAM", dbus_period, mc_period, t_rp, t_rcd, t_cas, t_ras, refresh_period, rq_size, wq_size, chans,
                                                   chan_width, rows, columns, ranks, bankgroups, banks, refreshes_per_period})
{
}

DRAM_CHANNEL::DRAM_CHANNEL(champsim::chrono::picoseconds dbus_period, champsim::chrono::picoseconds mc_period, std::size_t t_rp, std::size_t t_rcd,
                           std::size_t t_cas, std::size_t t_ras, champsim::chrono::microseconds refresh_period, std::size_t refreshes_per_period,
                           champsim::data::bytes width, std::size_t rq_size, std::size_t wq_size, DRAM_ADDRESS_MAPPING addr_mapper, std::string name,
                           champsim::address address_offset_)
    : champsim::operable(mc_period), channel_name(std::move(name)), address_mapping(addr_mapper), address_offset(address_offset_), WQ{wq_size}, RQ{rq_size},
      channel_width(width),
      DRAM_ROWS_PER_REFRESH(address_mapping.rows() / refreshes_per_period), tRP(t_rp * mc_period), tRCD(t_rcd * mc_period), tCAS(t_cas * mc_period),
      tRAS(t_ras * mc_period), tREF(refresh_period / refreshes_per_period),
      tRFC(std::chrono::duration_cast<champsim::chrono::clock::duration>(
          std::sqrt(champsim::data::bits_per_byte * (double)champsim::data::gibibytes{density()}.count()) * mc_period * t_ras)),
      DRAM_DBUS_TURN_AROUND_TIME(tRAS),
      DRAM_DBUS_RETURN_TIME(std::chrono::duration_cast<champsim::chrono::clock::duration>(dbus_period * address_mapping.prefetch_size)),
      DRAM_DBUS_BANKGROUP_STALL(
          std::chrono::duration_cast<champsim::chrono::clock::duration>((dbus_period * std::max(address_mapping.prefetch_size / 3, std::size_t{1})))),
      data_bus_period(dbus_period)
{
  request_array_type br(address_mapping.ranks() * address_mapping.banks() * address_mapping.bankgroups());
  bank_request = br;
  active_request = std::end(bank_request);
}

DRAM_ADDRESS_MAPPING::DRAM_ADDRESS_MAPPING(champsim::data::bytes channel_width_, std::size_t pref_size_, std::size_t channels_, std::size_t bankgroups_,
                                           std::size_t banks_, std::size_t columns_, std::size_t ranks_, std::size_t rows_)
    : address_slicer(make_slicer(channel_width_, pref_size_, channels_, bankgroups_, banks_, columns_, ranks_, rows_)), prefetch_size(pref_size_)
{
  // assert prefetch size is not zero
  assert(prefetch_size != 0);
  // assert prefetch size is multiple of block size
  assert((channel_width_.count() * prefetch_size) % BLOCK_SIZE == 0);

  // mapping sanity check
  assert(columns() >= 1 && columns() == columns_);
  assert(rows() >= 1 && rows() == rows_);
  assert(banks() >= 1 && banks() == banks_);
  assert(bankgroups() >= 1 && bankgroups() == bankgroups_);
  assert(ranks() >= 1 && ranks() == ranks_);
  assert(channels() >= 1 && channels() == channels_);
}

auto DRAM_ADDRESS_MAPPING::make_slicer(champsim::data::bytes channel_width, std::size_t pref_size, std::size_t channels, std::size_t bankgroups,
                                       std::size_t banks, std::size_t columns, std::size_t ranks, std::size_t rows) -> slicer_type
{
  std::array<std::size_t, slicer_type::size()> params{};
  params.at(SLICER_ROW_IDX) = rows;
  params.at(SLICER_COLUMN_IDX) = columns / pref_size;
  params.at(SLICER_RANK_IDX) = ranks;
  params.at(SLICER_BANK_IDX) = banks;
  params.at(SLICER_BANKGROUP_IDX) = bankgroups;
  params.at(SLICER_CHANNEL_IDX) = channels;
  params.at(SLICER_OFFSET_IDX) = channel_width.count() * pref_size;
  return std::apply([](auto... p) { return champsim::make_contiguous_extent_set(0, champsim::lg2(p)...); }, params);
}

long MEMORY_CONTROLLER::operate()
{
  long progress{0};

  initiate_requests();

  for (auto& channel : channels) {
    progress += channel._operate();
  }

  return progress;
}

long DRAM_CHANNEL::operate()
{
  long progress{0};

  if (warmup) {
    for (auto& entry : RQ) {
      if (entry.has_value()) {
        response_type response{entry->address, entry->v_address, entry->data, entry->pf_metadata, entry->instr_depend_on_me};
        for (auto* ret : entry.value().to_return) {
          ret->push_back(response);
        }

        ++progress;
        entry.reset();
      }
    }

    for (auto& entry : WQ) {
      if (entry.has_value()) {
        ++progress;
      }
      entry.reset();
    }
  }

  auto rq_occu = static_cast<uint64_t>(std::count_if(std::begin(RQ), std::end(RQ), [](const auto& x) { return x.has_value(); }));
  auto wq_occu = static_cast<uint64_t>(std::count_if(std::begin(WQ), std::end(WQ), [](const auto& x) { return x.has_value(); }));
  auto total_occu = rq_occu + wq_occu;
  ++sim_stats.cycles_elapsed;
  sim_stats.theoretical_max_bytes += static_cast<double>(channel_width.count()) * static_cast<double>(clock_period.count()) / static_cast<double>(data_bus_period.count());
  sim_stats.total_rq_queue_occupancy += rq_occu;
  sim_stats.total_wq_queue_occupancy += wq_occu;
  sim_stats.peak_rq_queue_occupancy = std::max(sim_stats.peak_rq_queue_occupancy, rq_occu);
  sim_stats.peak_wq_queue_occupancy = std::max(sim_stats.peak_wq_queue_occupancy, wq_occu);
  sim_stats.peak_total_queue_occupancy = std::max(sim_stats.peak_total_queue_occupancy, total_occu);

  check_write_collision();
  check_read_collision();
  progress += finish_dbus_request();
  swap_write_mode();
  progress += schedule_refresh();
  progress += populate_dbus();
  progress += service_packet(schedule_packet());

  return progress;
}

long DRAM_CHANNEL::finish_dbus_request()
{
  long progress{0};

  if (active_request != std::end(bank_request) && active_request->ready_time <= current_time) {
    response_type response{active_request->pkt->value().address, active_request->pkt->value().v_address, active_request->pkt->value().data,
                           active_request->pkt->value().pf_metadata, active_request->pkt->value().instr_depend_on_me};
    for (auto* ret : active_request->pkt->value().to_return) {
      ret->push_back(response);
    }

    sim_stats.bytes_transferred += BLOCK_SIZE;
    if (active_request->is_write) {
      ++sim_stats.write_requests;
    } else {
      ++sim_stats.read_requests;
      sim_stats.bytes_returned += BLOCK_SIZE;
      if (active_request->pkt->value().type != access_type::PREFETCH) {
        ++sim_stats.demand_requests;
        ++sim_stats.demand_tier_accesses;
        sim_stats.total_demand_latency_cycles += (current_time - active_request->pkt->value().time_enqueued) / clock_period;
      }
    }

    active_request->valid = false;

    active_request->pkt->reset();
    active_request = std::end(bank_request);
    ++progress;
  }

  return progress;
}

champsim::address DRAM_CHANNEL::local_address(champsim::address addr) const { return subtract_offset(addr, address_offset); }

long DRAM_CHANNEL::schedule_refresh()
{
  long progress = {0};
  // check if we reached refresh cycle

  bool schedule_refresh = current_time >= last_refresh + tREF;
  // if so, record stats
  if (schedule_refresh) {
    last_refresh = current_time;
    refresh_row += DRAM_ROWS_PER_REFRESH;
    sim_stats.refresh_cycles++;
    if (refresh_row >= address_mapping.rows())
      refresh_row -= address_mapping.rows();
  }

  // go through each bank, and handle refreshes
  for (auto& b_req : bank_request) {
    // refresh is now needed for this bank
    if (schedule_refresh) {
      b_req.need_refresh = true;
    }
    // refresh is being scheduled for this bank
    if (b_req.need_refresh && !b_req.valid) {
      b_req.ready_time = current_time + tRFC;
      b_req.need_refresh = false;
      b_req.under_refresh = true;
    }
    // refresh is done for this bank
    else if (b_req.under_refresh && b_req.ready_time <= current_time) {
      b_req.under_refresh = false;
      b_req.open_row.reset();
      progress++;
    }

    if (b_req.under_refresh)
      progress++;
  }
  return (progress);
}

void DRAM_CHANNEL::swap_write_mode()
{
  // these values control when to send out a burst of writes
  const std::size_t DRAM_WRITE_HIGH_WM = ((std::size(WQ) * 7) >> 3); // 7/8th
  const std::size_t DRAM_WRITE_LOW_WM = ((std::size(WQ) * 6) >> 3);  // 6/8th
  // const std::size_t MIN_DRAM_WRITES_PER_SWITCH = ((std::size(WQ) * 1) >> 2); // 1/4

  // Check queue occupancy
  auto wq_occu = static_cast<std::size_t>(std::count_if(std::begin(WQ), std::end(WQ), [](const auto& x) { return x.has_value(); }));
  auto rq_occu = static_cast<std::size_t>(std::count_if(std::begin(RQ), std::end(RQ), [](const auto& x) { return x.has_value(); }));

  // Change modes if the queues are unbalanced
  if ((!write_mode && (wq_occu >= DRAM_WRITE_HIGH_WM || (rq_occu == 0 && wq_occu > 0)))
      || (write_mode && (wq_occu == 0 || (rq_occu > 0 && wq_occu < DRAM_WRITE_LOW_WM)))) {
    // Reset scheduled requests
    for (auto it = std::begin(bank_request); it != std::end(bank_request); ++it) {
      // Leave active request on the data bus
      if (it != active_request && it->valid) {
        // Leave rows charged
        if (it->ready_time < (current_time + tCAS)) {
          it->open_row.reset();
        }

        // This bank is ready for another DRAM request
        it->valid = false;
        it->pkt->value().scheduled = false;
        it->pkt->value().ready_time = current_time;
      }
    }

    // Add data bus turn-around time
    if (active_request != std::end(bank_request)) {
      dbus_cycle_available = active_request->ready_time + DRAM_DBUS_TURN_AROUND_TIME; // After ongoing finish
    } else {
      dbus_cycle_available = current_time + DRAM_DBUS_TURN_AROUND_TIME;
    }

    // Invert the mode
    write_mode = !write_mode;
  }
}

// Look for requests to put on the bus
long DRAM_CHANNEL::populate_dbus()
{
  long progress{0};

  auto iter_next_process = std::min_element(std::begin(bank_request), std::end(bank_request),
                                            [](const auto& lhs, const auto& rhs) { return !rhs.valid || (lhs.valid && lhs.ready_time < rhs.ready_time); });
  if (iter_next_process->valid && iter_next_process->ready_time <= current_time) {
    if (active_request == std::end(bank_request) && dbus_cycle_available <= current_time) {
      // Bus is available
      // Put this request on the data bus

      // get which bankgroup we are in
      auto op_bankgroup = bankgroup_request_index(iter_next_process->pkt->value().address);
      auto bankgroup_ready_time = bankgroup_readytime[op_bankgroup];

      active_request = iter_next_process;

      // set return time. Incur penalty if bankgroup is on cooldown
      if (bankgroup_ready_time > current_time)
        active_request->ready_time = bankgroup_ready_time + DRAM_DBUS_RETURN_TIME;
      else
        active_request->ready_time = current_time + DRAM_DBUS_RETURN_TIME;

      // set when bankgroup dbus will be next ready
      bankgroup_readytime[op_bankgroup] = current_time + DRAM_DBUS_RETURN_TIME + DRAM_DBUS_BANKGROUP_STALL;

      if (iter_next_process->row_buffer_hit) {
        if (write_mode) {
          ++sim_stats.WQ_ROW_BUFFER_HIT;
        } else {
          ++sim_stats.RQ_ROW_BUFFER_HIT;
        }
      } else if (write_mode) {
        ++sim_stats.WQ_ROW_BUFFER_MISS;
      } else {
        ++sim_stats.RQ_ROW_BUFFER_MISS;
      }

      ++progress;
    } else {
      // Bus is congested
      if (active_request != std::end(bank_request)) {
        sim_stats.dbus_cycle_congested += (active_request->ready_time - current_time) / data_bus_period;
      } else {
        sim_stats.dbus_cycle_congested += (dbus_cycle_available - current_time) / data_bus_period;
      }
      ++sim_stats.dbus_count_congested;
    }
  }

  return progress;
}

std::size_t DRAM_CHANNEL::bank_request_index(champsim::address addr) const
{
  auto op_bank = address_mapping.get_bank(local_address(addr));

  return (bankgroup_request_index(addr) * address_mapping.banks() + op_bank);
}

std::size_t DRAM_CHANNEL::bankgroup_request_index(champsim::address addr) const
{
  auto local = local_address(addr);
  auto op_rank = address_mapping.get_rank(local);
  auto op_bankgroup = address_mapping.get_bankgroup(local);

  return (op_rank * address_mapping.bankgroups() + op_bankgroup);
}

// Look for queued packets that have not been scheduled
DRAM_CHANNEL::queue_type::iterator DRAM_CHANNEL::schedule_packet()
{
  // Look for queued packets that have not been scheduled
  // prioritize packets that are ready to execute, bank is free
  auto next_schedule = [this](const auto& lhs, const auto& rhs) {
    if (!(rhs.has_value() && !rhs.value().scheduled)) {
      return true;
    }
    if (!(lhs.has_value() && !lhs.value().scheduled)) {
      return false;
    }

    auto lop_idx = this->bank_request_index(lhs.value().address);
    auto rop_idx = this->bank_request_index(rhs.value().address);
    auto rready = !this->bank_request[rop_idx].valid;
    auto lready = !this->bank_request[lop_idx].valid;
    return (rready == lready) ? lhs.value().ready_time <= rhs.value().ready_time : lready;
  };
  queue_type::iterator iter_next_schedule;
  if (write_mode) {
    iter_next_schedule = std::min_element(std::begin(WQ), std::end(WQ), next_schedule);
  } else {
    iter_next_schedule = std::min_element(std::begin(RQ), std::end(RQ), next_schedule);
  }
  return (iter_next_schedule);
}

long DRAM_CHANNEL::service_packet(DRAM_CHANNEL::queue_type::iterator pkt)
{
  long progress{0};
  if (pkt->has_value() && pkt->value().ready_time <= current_time) {
    auto op_row = address_mapping.get_row(local_address(pkt->value().address));
    auto op_idx = bank_request_index(pkt->value().address);

    if (!bank_request[op_idx].valid && !bank_request[op_idx].under_refresh) {
      bool row_buffer_hit = (bank_request[op_idx].open_row.has_value() && *(bank_request[op_idx].open_row) == op_row);

      // this bank is now busy
      auto row_charge_delay = champsim::chrono::clock::duration{bank_request[op_idx].open_row.has_value() ? tRP + tRCD : tRCD};
      bank_request[op_idx] = {true, row_buffer_hit, false, false, write_mode, std::optional{op_row},
                              current_time + tCAS + (row_buffer_hit ? champsim::chrono::clock::duration{} : row_charge_delay), pkt};
      pkt->value().scheduled = true;
      pkt->value().ready_time = champsim::chrono::clock::time_point::max();

      ++progress;
    }
  }

  return progress;
}

void MEMORY_CONTROLLER::initialize()
{
  using namespace champsim::data::data_literals;
  using namespace std::literals::chrono_literals;
  auto print_tier = [](std::string_view name, champsim::data::bytes sz, std::size_t channel_count, champsim::data::bytes width,
                       champsim::chrono::picoseconds bus_period) {
    if (champsim::data::gibibytes gb_sz{sz}; gb_sz > 1_GiB) {
      fmt::print("{} Size: {}", name, gb_sz);
    } else if (champsim::data::mebibytes mb_sz{sz}; mb_sz > 1_MiB) {
      fmt::print("{} Size: {}", name, mb_sz);
    } else if (champsim::data::kibibytes kb_sz{sz}; kb_sz > 1_kiB) {
      fmt::print("{} Size: {}", name, kb_sz);
    } else {
      fmt::print("{} Size: {}", name, sz);
    }

    fmt::print(" Channels: {} Width: {}-bit Data Rate: {} MT/s\n", channel_count, champsim::data::bits_per_byte * width.count(), 1us / bus_period);
  };

  print_tier(primary_name, primary_size_bytes, primary_channel_count, primary_channel_width, data_bus_period);
  if (has_secondary_tier() && secondary_name.has_value() && secondary_channel_width.has_value()) {
    auto secondary_bus_period = channels.at(primary_channel_count).data_bus_period;
    print_tier(*secondary_name, secondary_size_bytes, std::size(channels) - primary_channel_count, *secondary_channel_width, secondary_bus_period);
  }
}

void DRAM_CHANNEL::initialize() {}

void MEMORY_CONTROLLER::begin_phase()
{
  for (std::size_t i = 0; i < channels.size(); ++i) {
    auto& chan = channels[i];
    DRAM_CHANNEL::stats_type new_stats;
    new_stats.name = chan.channel_name;
    new_stats.rq_capacity = std::size(chan.RQ);
    new_stats.wq_capacity = std::size(chan.WQ);
    new_stats.is_secondary = (i >= primary_channel_count);
    chan.sim_stats = new_stats;
    chan.warmup = warmup;
  }

  for (auto* ul : queues) {
    channel_type::stats_type ul_new_roi_stats;
    channel_type::stats_type ul_new_sim_stats;
    ul->roi_stats = ul_new_roi_stats;
    ul->sim_stats = ul_new_sim_stats;
  }
}

void DRAM_CHANNEL::begin_phase() {}

void MEMORY_CONTROLLER::end_phase(unsigned cpu)
{
  for (auto& chan : channels) {
    chan.end_phase(cpu);
  }
}

void DRAM_CHANNEL::end_phase(unsigned /*cpu*/) { roi_stats = sim_stats; }

bool DRAM_ADDRESS_MAPPING::is_collision(champsim::address a, champsim::address b) const
{
  // collision if everything but offset matches
  champsim::data::bits offset_bits = champsim::data::bits{champsim::size(get<SLICER_OFFSET_IDX>(address_slicer))};
  return (a.slice_upper(offset_bits) == b.slice_upper(offset_bits));
}

void DRAM_CHANNEL::check_write_collision()
{
  for (auto wq_it = std::begin(WQ); wq_it != std::end(WQ); ++wq_it) {
    if (wq_it->has_value() && !wq_it->value().forward_checked) {
      auto checker = [this, check_val = local_address(wq_it->value().address)](const auto& pkt) {
        return pkt.has_value() && address_mapping.is_collision(local_address(pkt.value().address), check_val);
      };

      auto found = std::find_if(std::begin(WQ), wq_it, checker); // Forward check
      if (found == wq_it) {
        found = std::find_if(std::next(wq_it), std::end(WQ), checker); // Backward check
      }

      if (found != std::end(WQ)) {
        wq_it->reset();
      } else {
        wq_it->value().forward_checked = true;
      }
    }
  }
}

void DRAM_CHANNEL::check_read_collision()
{
  for (auto rq_it = std::begin(RQ); rq_it != std::end(RQ); ++rq_it) {
    if (rq_it->has_value() && !rq_it->value().forward_checked) {
      auto checker = [this, check_val = local_address(rq_it->value().address)](const auto& x) {
        return x.has_value() && address_mapping.is_collision(local_address(x.value().address), check_val);
      };
      // write forward
      if (auto wq_it = std::find_if(std::begin(WQ), std::end(WQ), checker); wq_it != std::end(WQ)) {
        response_type response{rq_it->value().address, rq_it->value().v_address, wq_it->value().data, rq_it->value().pf_metadata,
                               rq_it->value().instr_depend_on_me};
        for (auto* ret : rq_it->value().to_return) {
          ret->push_back(response);
        }

        rq_it->reset();

      }
      // backwards check
      else if (auto found = std::find_if(std::begin(RQ), rq_it, checker); found != rq_it) {
        auto instr_copy = std::move(found->value().instr_depend_on_me);
        auto ret_copy = std::move(found->value().to_return);

        std::set_union(std::begin(instr_copy), std::end(instr_copy), std::begin(rq_it->value().instr_depend_on_me), std::end(rq_it->value().instr_depend_on_me),
                       std::back_inserter(found->value().instr_depend_on_me));
        std::set_union(std::begin(ret_copy), std::end(ret_copy), std::begin(rq_it->value().to_return), std::end(rq_it->value().to_return),
                       std::back_inserter(found->value().to_return));

        rq_it->reset();

      }
      // forwards check
      else if (found = std::find_if(std::next(rq_it), std::end(RQ), checker); found != std::end(RQ)) {
        auto instr_copy = std::move(found->value().instr_depend_on_me);
        auto ret_copy = std::move(found->value().to_return);

        std::set_union(std::begin(instr_copy), std::end(instr_copy), std::begin(rq_it->value().instr_depend_on_me), std::end(rq_it->value().instr_depend_on_me),
                       std::back_inserter(found->value().instr_depend_on_me));
        std::set_union(std::begin(ret_copy), std::end(ret_copy), std::begin(rq_it->value().to_return), std::end(rq_it->value().to_return),
                       std::back_inserter(found->value().to_return));

        rq_it->reset();
      } else {
        rq_it->value().forward_checked = true;
      }
    }
  }
}

void MEMORY_CONTROLLER::initiate_requests()
{
  // Initiate read requests
  for (auto* ul : queues) {
    for (auto q : {std::ref(ul->RQ), std::ref(ul->PQ)}) {
      auto [begin, end] = champsim::get_span_p(std::cbegin(q.get()), std::cend(q.get()), [ul, this](const auto& pkt) { return this->add_rq(pkt, ul); });
      q.get().erase(begin, end);
    }

    // Initiate write requests
    auto [wq_begin, wq_end] = champsim::get_span_p(std::cbegin(ul->WQ), std::cend(ul->WQ), [this](const auto& pkt) { return this->add_wq(pkt); });
    ul->WQ.erase(wq_begin, wq_end);
  }
}

DRAM_CHANNEL::request_type::request_type(const typename champsim::channel::request_type& req)
    : pf_metadata(req.pf_metadata), type(req.type), address(req.address), v_address(req.address), data(req.data), instr_depend_on_me(req.instr_depend_on_me)
{
  asid[0] = req.asid[0];
  asid[1] = req.asid[1];
}

bool MEMORY_CONTROLLER::has_secondary_tier() const { return secondary_address_mapping.has_value() && secondary_size_bytes.count() > 0; }

bool MEMORY_CONTROLLER::is_secondary_address(champsim::address address) const
{
  return has_secondary_tier() && address.to<uint64_t>() >= static_cast<uint64_t>(primary_size_bytes.count());
}

champsim::address MEMORY_CONTROLLER::normalize_secondary_address(champsim::address address) const
{
  return subtract_offset(address, champsim::lowest_address_for_size(primary_size_bytes));
}

std::size_t MEMORY_CONTROLLER::channel_index(champsim::address address) const
{
  if (is_secondary_address(address) && secondary_address_mapping.has_value()) {
    return primary_channel_count + secondary_address_mapping->get_channel(normalize_secondary_address(address));
  }

  return address_mapping.get_channel(address);
}

bool MEMORY_CONTROLLER::add_rq(const request_type& packet, champsim::channel* ul)
{
  // Translate the CPU-visible address to the routed DRAM slot (may differ after migration).
  auto routed_addr = placement_table.get_routed_address(packet.address);
  auto& channel = channels[channel_index(routed_addr)];

  if (auto rq_it = std::find_if_not(std::begin(channel.RQ), std::end(channel.RQ), [this](const auto& pkt) { return pkt.has_value(); });
      rq_it != std::end(channel.RQ)) {
    *rq_it = DRAM_CHANNEL::request_type{packet};
    rq_it->value().address = routed_addr;  // DRAM channel uses routed addr for bank/row mapping
    rq_it->value().forward_checked = false;
    rq_it->value().scheduled = false;
    rq_it->value().ready_time = channel.current_time;
    // Use the channel's own current_time so latency is measured in the channel's
    // clock domain. Using MEMORY_CONTROLLER::current_time causes a systematic drift
    // when the secondary (CXL) channel has a different clock period than the primary.
    rq_it->value().time_enqueued = channel.current_time;
    if (packet.response_requested)
      rq_it->value().to_return = {&ul->returned};

    return true;
  }

  return false;
}

bool MEMORY_CONTROLLER::add_wq(const request_type& packet)
{
  // Translate the CPU-visible address to the routed DRAM slot (may differ after migration).
  auto routed_addr = placement_table.get_routed_address(packet.address);
  auto& channel = channels[channel_index(routed_addr)];

  // search for the empty index
  if (auto wq_it = std::find_if_not(std::begin(channel.WQ), std::end(channel.WQ), [](const auto& pkt) { return pkt.has_value(); });
      wq_it != std::end(channel.WQ)) {
    *wq_it = DRAM_CHANNEL::request_type{packet};
    wq_it->value().address = routed_addr;  // DRAM channel uses routed addr for bank/row mapping
    wq_it->value().forward_checked = false;
    wq_it->value().scheduled = false;
    wq_it->value().ready_time = channel.current_time;
    wq_it->value().time_enqueued = channel.current_time;  // same fix as add_rq

    return true;
  }

  ++channel.sim_stats.WQ_FULL;
  return false;
}

unsigned long DRAM_ADDRESS_MAPPING::swizzle_bits(champsim::address address, unsigned long segment_size, champsim::data::bits segment_offset,
                                                 unsigned long field, unsigned long field_bits) const
{
  champsim::address_slice row{get<SLICER_ROW_IDX>(address_slicer), address};
  unsigned long permute_field = field;

  for (champsim::dynamic_extent subextent{champsim::data::bits{0}, segment_size}; subextent.upper <= row.upper_extent();
       subextent = champsim::dynamic_extent{subextent.upper, segment_size}) {
    permute_field ^= row.slice(subextent).slice(champsim::dynamic_extent{segment_offset, field_bits}).to<unsigned long>();
  }
  return permute_field;
}

unsigned long DRAM_ADDRESS_MAPPING::get_channel(champsim::address address) const
{
  unsigned long channel = std::get<SLICER_CHANNEL_IDX>(address_slicer(address)).to<unsigned long>();
  // channel bits should be xor'd with each row bit
  unsigned long c_bits = champsim::size(get<SLICER_CHANNEL_IDX>(address_slicer));
  return (swizzle_bits(address, 1, champsim::data::bits{0}, channel, c_bits));
}
unsigned long DRAM_ADDRESS_MAPPING::get_rank(champsim::address address) const { return std::get<SLICER_RANK_IDX>(address_slicer(address)).to<unsigned long>(); }
unsigned long DRAM_ADDRESS_MAPPING::get_bankgroup(champsim::address address) const
{
  unsigned long bankgroup = std::get<SLICER_BANKGROUP_IDX>(address_slicer(address)).to<unsigned long>();

  unsigned long bg_bits = champsim::size(get<SLICER_BANKGROUP_IDX>(address_slicer));
  unsigned long bk_bits = champsim::size(get<SLICER_BANK_IDX>(address_slicer));
  return (swizzle_bits(address, bg_bits + bk_bits, champsim::data::bits{0}, bankgroup, bg_bits));
}
unsigned long DRAM_ADDRESS_MAPPING::get_bank(champsim::address address) const
{
  unsigned long bank = std::get<SLICER_BANK_IDX>(address_slicer(address)).to<unsigned long>();

  unsigned long bg_bits = champsim::size(get<SLICER_BANKGROUP_IDX>(address_slicer));
  unsigned long bk_bits = champsim::size(get<SLICER_BANK_IDX>(address_slicer));
  // bank bits should be xor'd with select row bits

  return (swizzle_bits(address, bg_bits + bk_bits, champsim::data::bits{bg_bits}, bank, bk_bits));
}
unsigned long DRAM_ADDRESS_MAPPING::get_row(champsim::address address) const { return std::get<SLICER_ROW_IDX>(address_slicer(address)).to<unsigned long>(); }
unsigned long DRAM_ADDRESS_MAPPING::get_column(champsim::address address) const
{
  return std::get<SLICER_COLUMN_IDX>(address_slicer(address)).to<unsigned long>();
}

champsim::data::bytes MEMORY_CONTROLLER::size() const { return primary_size_bytes + secondary_size_bytes; }
champsim::data::bytes MEMORY_CONTROLLER::primary_size() const { return primary_size_bytes; }
champsim::data::bytes MEMORY_CONTROLLER::secondary_size() const { return secondary_size_bytes; }
bool MEMORY_CONTROLLER::is_tiered() const { return has_secondary_tier(); }
champsim::data::bytes DRAM_CHANNEL::density() const
{
  return champsim::data::bytes{(long long)(address_mapping.rows() * address_mapping.columns() * address_mapping.banks() * address_mapping.bankgroups())};
}

std::size_t DRAM_ADDRESS_MAPPING::rows() const { return std::size_t{1} << champsim::size(get<SLICER_ROW_IDX>(address_slicer)); }
std::size_t DRAM_ADDRESS_MAPPING::columns() const { return prefetch_size << champsim::size(get<SLICER_COLUMN_IDX>(address_slicer)); }
std::size_t DRAM_ADDRESS_MAPPING::ranks() const { return std::size_t{1} << champsim::size(get<SLICER_RANK_IDX>(address_slicer)); }
std::size_t DRAM_ADDRESS_MAPPING::bankgroups() const { return std::size_t{1} << champsim::size(get<SLICER_BANKGROUP_IDX>(address_slicer)); }
std::size_t DRAM_ADDRESS_MAPPING::banks() const { return std::size_t{1} << champsim::size(get<SLICER_BANK_IDX>(address_slicer)); }
std::size_t DRAM_ADDRESS_MAPPING::channels() const { return std::size_t{1} << champsim::size(get<SLICER_CHANNEL_IDX>(address_slicer)); }
std::size_t DRAM_CHANNEL::bank_request_capacity() const { return std::size(bank_request); }
std::size_t DRAM_CHANNEL::bankgroup_request_capacity() const { return std::size(bankgroup_readytime); };

// LCOV_EXCL_START Exclude the following function from LCOV
void MEMORY_CONTROLLER::print_deadlock()
{
  for (auto& chan : channels) {
    fmt::print("{}\n", chan.channel_name);
    chan.print_deadlock();
  }
}

void DRAM_CHANNEL::print_deadlock()
{
  std::string_view q_writer{"address: {} forward_checked: {} scheduled: {}"};
  auto q_entry_pack = [](const auto& entry) {
    return std::tuple{entry->address, entry->forward_checked, entry->scheduled};
  };

  champsim::range_print_deadlock(RQ, "RQ", q_writer, q_entry_pack);
  champsim::range_print_deadlock(WQ, "WQ", q_writer, q_entry_pack);
}
// LCOV_EXCL_STOP
