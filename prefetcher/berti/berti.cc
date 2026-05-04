#include "berti.h"

#include <algorithm>
#include <fmt/core.h>

#include "cache.h"

uint64_t berti::current_cycle() const
{
  if (intern_->clock_period.count() == 0)
    return 0;

  return static_cast<uint64_t>(intern_->current_time.time_since_epoch() / intern_->clock_period);
}

uint64_t berti::line_number(champsim::address addr)
{
  return addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;
}

champsim::address berti::address_from_line(uint64_t line)
{
  return champsim::address{line << LOG2_BLOCK_SIZE};
}

uint64_t berti::ip_hash(champsim::address ip)
{
  const auto raw_ip = ip.to<uint64_t>();
  return raw_ip ^ (raw_ip >> 16) ^ (raw_ip >> 32);
}

std::size_t berti::history_set_index(champsim::address ip)
{
  return ip.to<uint64_t>() % HISTORY_SETS;
}

uint64_t berti::history_ip_tag(champsim::address ip)
{
  const auto mask = (uint64_t{1} << HISTORY_IP_TAG_BITS) - 1;
  return (ip.to<uint64_t>() >> 3) & mask;
}

uint64_t berti::delta_table_ip_tag(champsim::address ip)
{
  const auto mask = (uint64_t{1} << DELTA_TABLE_IP_TAG_BITS) - 1;
  return ip_hash(ip) & mask;
}

uint64_t berti::history_line(uint64_t line)
{
  const auto mask = (uint64_t{1} << HISTORY_LINE_BITS) - 1;
  return line & mask;
}

uint64_t berti::history_timestamp(uint64_t cycle)
{
  return cycle & HISTORY_TIMESTAMP_MASK;
}

uint64_t berti::history_timestamp_distance(uint64_t older_timestamp, uint64_t newer_cycle)
{
  return (history_timestamp(newer_cycle) - older_timestamp) & HISTORY_TIMESTAMP_MASK;
}

bool berti::history_timestamp_not_after(uint64_t timestamp, uint64_t cycle)
{
  return history_timestamp_distance(timestamp, cycle) < HISTORY_TIMESTAMP_HALF_RANGE;
}

uint64_t berti::elapsed_cycles(uint64_t begin, uint64_t end)
{
  return end >= begin ? end - begin : 0;
}

uint64_t berti::stored_latency(uint64_t latency)
{
  return latency <= MAX_STORED_LATENCY ? latency : 0;
}

bool berti::is_demand(access_type type)
{
  return type == access_type::LOAD || type == access_type::RFO;
}

void berti::add_history(champsim::address ip, uint64_t line, uint64_t cycle)
{
  auto& set = history.at(history_set_index(ip));
  auto& victim = set.entries.at(set.next_victim);
  if (victim.valid)
    ++stats.history_replacements;

  victim = history_entry{true, history_ip_tag(ip), history_line(line), history_timestamp(cycle)};
  set.next_victim = (set.next_victim + 1) % HISTORY_WAYS;
  ++stats.history_inserts;
}

std::vector<int64_t> berti::find_timely_deltas(champsim::address ip, uint64_t line, uint64_t latency, uint64_t cycle) const
{
  if (latency == 0 || latency > cycle)
    return {};

  struct candidate {
    history_entry entry;
    uint64_t age = 0;
  };

  const auto ready_cycle = cycle - latency;
  const auto tag = history_ip_tag(ip);
  const auto current_line = static_cast<int64_t>(history_line(line));
  const auto& set = history.at(history_set_index(ip));

  std::vector<candidate> candidates;
  candidates.reserve(HISTORY_WAYS);
  for (const auto& entry : set.entries) {
    if (entry.valid && entry.ip_tag == tag && history_timestamp_not_after(entry.timestamp, ready_cycle))
      candidates.push_back(candidate{entry, history_timestamp_distance(entry.timestamp, cycle)});
  }

  std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) { return lhs.age < rhs.age; });

  std::vector<int64_t> deltas;
  deltas.reserve(MAX_TIMELY_DELTAS);
  for (const auto& candidate : candidates) {
    if (deltas.size() >= MAX_TIMELY_DELTAS)
      break;

    const auto delta = current_line - static_cast<int64_t>(candidate.entry.line);
    if (delta == 0 || delta < MIN_DELTA || delta > MAX_DELTA)
      continue;

    if (std::find(deltas.begin(), deltas.end(), delta) != deltas.end())
      continue;

    deltas.push_back(delta);
  }

  return deltas;
}

void berti::record_timely_delta_search(champsim::address ip, uint64_t line, uint64_t latency, uint64_t cycle)
{
  if (latency == 0 || latency > cycle)
    return;

  const auto timely = find_timely_deltas(ip, line, latency, cycle);
  ++stats.history_searches;
  stats.timely_deltas += timely.size();
}

berti::in_flight_entry* berti::find_in_flight(uint64_t line)
{
  auto found = std::find_if(in_flight.begin(), in_flight.end(), [line](const auto& entry) { return entry.valid && entry.line == line; });
  return found == in_flight.end() ? nullptr : &(*found);
}

berti::in_flight_entry& berti::get_or_allocate_in_flight(uint64_t line)
{
  if (auto* existing = find_in_flight(line); existing != nullptr)
    return *existing;

  auto invalid = std::find_if(in_flight.begin(), in_flight.end(), [](const auto& entry) { return !entry.valid; });
  if (invalid != in_flight.end()) {
    *invalid = in_flight_entry{};
    invalid->valid = true;
    invalid->line = line;
    return *invalid;
  }

  auto& victim = in_flight.at(next_in_flight_victim);
  next_in_flight_victim = (next_in_flight_victim + 1) % IN_FLIGHT_ENTRIES;
  victim = in_flight_entry{};
  victim.valid = true;
  victim.line = line;
  return victim;
}

void berti::record_demand_issue(uint64_t line, champsim::address ip, uint64_t cycle)
{
  auto& entry = get_or_allocate_in_flight(line);
  if (entry.demand_valid)
    return;

  entry.demand_valid = true;
  entry.demand_ip = ip;
  entry.demand_cycle = cycle;
  ++stats.demand_issue_records;
}

void berti::record_prefetch_issue(uint64_t line, uint64_t cycle)
{
  auto& entry = get_or_allocate_in_flight(line);
  entry.prefetch_valid = true;
  entry.prefetch_cycle = cycle;
  ++stats.prefetch_issue_records;
}

void berti::remove_demand_issue(in_flight_entry& entry)
{
  entry.demand_valid = false;
  if (!entry.prefetch_valid)
    entry.valid = false;
}

void berti::remove_prefetch_issue(in_flight_entry& entry)
{
  entry.prefetch_valid = false;
  if (!entry.demand_valid)
    entry.valid = false;
}

std::size_t berti::prefetch_latency_index(uint64_t line)
{
  return line % PREFETCH_LATENCY_ENTRIES;
}

void berti::remember_prefetch_latency(uint64_t line, uint64_t latency)
{
  latency = stored_latency(latency);
  if (latency == 0) {
    forget_prefetch_latency(line);
    return;
  }

  prefetch_latencies.at(prefetch_latency_index(line)) = prefetch_latency_entry{true, line, latency};
}

void berti::forget_prefetch_latency(uint64_t line)
{
  auto& entry = prefetch_latencies.at(prefetch_latency_index(line));
  if (entry.valid && entry.line == line)
    entry.valid = false;
}

uint64_t berti::consume_prefetch_latency(uint64_t line)
{
  auto& entry = prefetch_latencies.at(prefetch_latency_index(line));
  if (!entry.valid || entry.line != line)
    return 0;

  const auto latency = stored_latency(entry.latency);
  entry.valid = false;
  if (latency > 0)
    ++stats.prefetched_line_latency_uses;
  return latency;
}

void berti::prefetcher_initialize()
{
  fmt::print("Berti prefetcher initialized\n");
}

uint32_t berti::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                         uint32_t metadata_in)
{
  if (!is_demand(type))
    return metadata_in;

  const auto cycle = current_cycle();
  const auto line = line_number(addr);

  if (useful_prefetch) {
    const auto latency = consume_prefetch_latency(line);
    record_timely_delta_search(ip, line, latency, cycle);
  }

  if (!cache_hit || useful_prefetch)
    add_history(ip, line, cycle);

  if (!cache_hit)
    record_demand_issue(line, ip, cycle);

  return metadata_in;
}

uint32_t berti::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  const auto cycle = current_cycle();
  const auto line = line_number(addr);
  auto* entry = find_in_flight(line);

  if (evicted_addr.to<uint64_t>() != 0)
    forget_prefetch_latency(line_number(evicted_addr));

  if (entry != nullptr) {
    const auto demand_was_valid = entry->demand_valid;

    if (entry->demand_valid) {
      const auto latency = stored_latency(elapsed_cycles(entry->demand_cycle, cycle));
      if (latency > 0) {
        ++stats.demand_latency_samples;
        stats.demand_latency_cycles += latency;
      }
      record_timely_delta_search(entry->demand_ip, line, latency, cycle);
      remove_demand_issue(*entry);
    }

    if (prefetch && entry->prefetch_valid) {
      const auto latency = stored_latency(elapsed_cycles(entry->prefetch_cycle, cycle));
      if (latency > 0) {
        ++stats.prefetch_latency_samples;
        stats.prefetch_latency_cycles += latency;
      }
      if (!demand_was_valid)
        remember_prefetch_latency(line, latency);
      remove_prefetch_issue(*entry);
    } else if (!prefetch && entry->prefetch_valid) {
      remove_prefetch_issue(*entry);
    }
  }

  return metadata_in;
}

void berti::prefetcher_final_stats()
{
  fmt::print("Berti prefetcher statistics\n");
  fmt::print("  DEMAND_ISSUE_RECORDS:          {}\n", stats.demand_issue_records);
  fmt::print("  DEMAND_LATENCY_SAMPLES:        {}\n", stats.demand_latency_samples);
  fmt::print("  DEMAND_LATENCY_CYCLES:         {}\n", stats.demand_latency_cycles);
  fmt::print("  PREFETCH_ISSUE_RECORDS:        {}\n", stats.prefetch_issue_records);
  fmt::print("  PREFETCH_LATENCY_SAMPLES:      {}\n", stats.prefetch_latency_samples);
  fmt::print("  PREFETCH_LATENCY_CYCLES:       {}\n", stats.prefetch_latency_cycles);
  fmt::print("  PREFETCHED_LINE_LATENCY_USES:  {}\n", stats.prefetched_line_latency_uses);
  fmt::print("  HISTORY_INSERTS:               {}\n", stats.history_inserts);
  fmt::print("  HISTORY_REPLACEMENTS:          {}\n", stats.history_replacements);
  fmt::print("  HISTORY_SEARCHES:              {}\n", stats.history_searches);
  fmt::print("  TIMELY_DELTAS:                 {}\n", stats.timely_deltas);
}
