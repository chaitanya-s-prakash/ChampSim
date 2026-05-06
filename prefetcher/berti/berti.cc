#include "berti.h"

#include <algorithm>
#include <fmt/core.h>

#include "cache.h"
#include "coaware.h"

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
  // Co-aware Fix C1/C2: skip this history write if the page recently migrated.
  // Migration invalidates latency measurements — the timestamp recorded now would
  // be at the old tier's latency, corrupting timely-delta computation at the new tier.
  // The check is per-page (not per-IP), so only this specific page is suppressed;
  // other pages accessed by the same IP write normally (Fix C2).
  const uint64_t page_num = address_from_line(line).to<uint64_t>() >> LOG2_PAGE_SIZE;
  auto mig_it = coaware::recently_migrated_pages.find(page_num);
  if (mig_it != coaware::recently_migrated_pages.end()) {
    if (coaware::current_epoch - mig_it->second <= 1) {
      ++stats.history_skipped_migration;
      return;  // let Berti re-measure latency from scratch at the new tier
    }
    coaware::recently_migrated_pages.erase(mig_it);  // signal expired
  }

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

void berti::train(champsim::address ip, uint64_t line, uint64_t latency, uint64_t cycle)
{
  if (latency == 0 || latency > cycle)
    return;

  const auto timely = find_timely_deltas(ip, line, latency, cycle);
  ++stats.history_searches;
  stats.timely_deltas += timely.size();

  auto& entry = get_or_allocate_delta_entry(delta_table_ip_tag(ip));
  increment_learning_counter(entry.search_count);
  for (auto delta_value : timely) {
    auto* delta = get_or_allocate_delta(entry, delta_value);
    if (delta == nullptr) {
      ++stats.delta_discards;
      continue;
    }

    increment_learning_counter(delta->seen_this_round);
    ++stats.trained_deltas;
  }

  if (entry.search_count >= LEARNING_ROUNDS)
    classify(entry);
}

void berti::issue_prefetches(champsim::address ip, uint64_t line, uint64_t cycle)
{
  auto* entry = find_delta_entry(delta_table_ip_tag(ip));
  if (entry == nullptr) {
    ++stats.delta_table_misses;
    return;
  }

  ++stats.delta_table_hits;

  const auto candidates = selected_deltas(*entry);
  const auto l1_mshr_available = intern_->get_mshr_occupancy_ratio() < L1_MSHR_OCCUPANCY_THRESHOLD;
  for (const auto& delta : candidates) {
    uint64_t pf_line = 0;
    if (!target_line(line, delta.delta, pf_line))
      continue;

    if (auto* inflight = find_in_flight(pf_line); inflight != nullptr && inflight->prefetch_valid)
      continue;

    const auto fill_l1 = delta.status == delta_status::l1_pref && l1_mshr_available;
    if (prefetch_line(address_from_line(pf_line), fill_l1, 0)) {
      record_prefetch_issue(pf_line, cycle);
      if (fill_l1)
        ++stats.prefetch_issued_l1;
      else
        ++stats.prefetch_issued_l2;
    }
  }
}

std::vector<berti::delta_entry> berti::selected_deltas(const ip_delta_entry& entry)
{
  std::vector<delta_entry> selected;
  selected.reserve(MAX_SELECTED_DELTAS);
  for (const auto& delta : entry.deltas) {
    if (delta.valid && selected_delta_status(delta.status))
      selected.push_back(delta);
  }

  add_warmup_deltas(entry, selected);

  std::sort(selected.begin(), selected.end(), [](const auto& lhs, const auto& rhs) {
    if (delta_status_priority(lhs.status) != delta_status_priority(rhs.status))
      return delta_status_priority(lhs.status) > delta_status_priority(rhs.status);
    return lhs.coverage > rhs.coverage;
  });

  if (selected.size() > MAX_SELECTED_DELTAS)
    selected.resize(MAX_SELECTED_DELTAS);
  return selected;
}

void berti::add_warmup_deltas(const ip_delta_entry& entry, std::vector<delta_entry>& selected)
{
  if (!warmup_prefetching_ready(entry))
    return;

  for (const auto& delta : entry.deltas) {
    if (!delta.valid)
      continue;

    const auto already_selected = std::find_if(selected.begin(), selected.end(), [&delta](const auto& candidate) { return candidate.delta == delta.delta; });
    if (already_selected != selected.end())
      continue;

    const auto coverage_percent = (100 * static_cast<uint64_t>(delta.seen_this_round)) / entry.search_count;
    if (coverage_percent < WARMUP_COVERAGE_PERCENT)
      continue;

    auto warmup_delta = delta;
    warmup_delta.coverage = delta.seen_this_round;
    warmup_delta.status = delta_status::l1_pref;
    selected.push_back(warmup_delta);
    ++stats.warmup_prefetch_candidates;
  }
}

bool berti::warmup_prefetching_ready(const ip_delta_entry& entry)
{
  return ENABLE_WARMUP_PREFETCHING && !entry.classified && entry.search_count >= WARMUP_MIN_SEARCHES;
}

bool berti::selected_delta_status(delta_status status)
{
  return status == delta_status::l1_pref || status == delta_status::l2_pref;
}

int berti::delta_status_priority(delta_status status)
{
  if (status == delta_status::l1_pref)
    return 3;
  if (status == delta_status::l2_pref)
    return 2;
  if (status == delta_status::l2_pref_repl)
    return 1;
  return 0;
}

bool berti::target_line(uint64_t line, int64_t delta, uint64_t& target)
{
  if (delta < 0) {
    const auto magnitude = static_cast<uint64_t>(-delta);
    if (line < magnitude)
      return false;
    target = line - magnitude;
    return true;
  }

  const auto magnitude = static_cast<uint64_t>(delta);
  if (line > UINT64_MAX - magnitude)
    return false;

  target = line + magnitude;
  return true;
}

berti::ip_delta_entry* berti::find_delta_entry(uint64_t ip_tag)
{
  auto found = std::find_if(delta_table.begin(), delta_table.end(), [ip_tag](const auto& entry) { return entry.valid && entry.ip_tag == ip_tag; });
  return found == delta_table.end() ? nullptr : &(*found);
}

berti::ip_delta_entry& berti::get_or_allocate_delta_entry(uint64_t ip_tag)
{
  if (auto* existing = find_delta_entry(ip_tag); existing != nullptr)
    return *existing;

  auto invalid = std::find_if(delta_table.begin(), delta_table.end(), [](const auto& entry) { return !entry.valid; });
  if (invalid != delta_table.end()) {
    *invalid = ip_delta_entry{};
    invalid->valid = true;
    invalid->ip_tag = ip_tag;
    ++stats.delta_table_inserts;
    return *invalid;
  }

  auto& victim = delta_table.at(next_delta_victim);
  next_delta_victim = (next_delta_victim + 1) % DELTA_TABLE_ENTRIES;
  victim = ip_delta_entry{};
  victim.valid = true;
  victim.ip_tag = ip_tag;
  ++stats.delta_table_replacements;
  return victim;
}

berti::delta_entry* berti::get_or_allocate_delta(ip_delta_entry& entry, int64_t delta)
{
  auto found = std::find_if(entry.deltas.begin(), entry.deltas.end(), [delta](const auto& candidate) { return candidate.valid && candidate.delta == delta; });
  if (found != entry.deltas.end())
    return &(*found);

  auto invalid = std::find_if(entry.deltas.begin(), entry.deltas.end(), [](const auto& candidate) { return !candidate.valid; });
  if (invalid != entry.deltas.end()) {
    *invalid = delta_entry{};
    invalid->valid = true;
    invalid->delta = delta;
    ++stats.delta_inserts;
    return &(*invalid);
  }

  auto* victim = find_delta_replacement(entry);
  if (victim == nullptr)
    return nullptr;

  *victim = delta_entry{};
  victim->valid = true;
  victim->delta = delta;
  ++stats.delta_replacements;
  return victim;
}

berti::delta_entry* berti::find_delta_replacement(ip_delta_entry& entry)
{
  auto victim = std::min_element(entry.deltas.begin(), entry.deltas.end(), [](const auto& lhs, const auto& rhs) {
    if (replaceable_delta_status(lhs.status) != replaceable_delta_status(rhs.status))
      return replaceable_delta_status(lhs.status);
    if (!replaceable_delta_status(lhs.status))
      return false;
    if (lhs.coverage != rhs.coverage)
      return lhs.coverage < rhs.coverage;
    return lhs.status == delta_status::none && rhs.status != delta_status::none;
  });

  return victim != entry.deltas.end() && replaceable_delta_status(victim->status) ? &(*victim) : nullptr;
}

bool berti::replaceable_delta_status(delta_status status)
{
  return status == delta_status::none || status == delta_status::l2_pref_repl;
}

void berti::classify(ip_delta_entry& entry)
{
  for (auto& delta : entry.deltas) {
    if (!delta.valid)
      continue;

    delta.coverage = delta.seen_this_round;
    if (delta.coverage > L1_COVERAGE_THRESHOLD)
      delta.status = delta_status::l1_pref;
    else if (delta.coverage >= L2_REPLACEABLE_COVERAGE_THRESHOLD)
      delta.status = delta_status::l2_pref;
    else if (delta.coverage > L2_COVERAGE_THRESHOLD)
      delta.status = delta_status::l2_pref_repl;
    else
      delta.status = delta_status::none;

    delta.seen_this_round = 0;
  }

  limit_selected_deltas(entry);
  entry.search_count = 0;
  entry.classified = true;
  ++stats.delta_classifications;

  // Co-aware: update ip_max_coverage for this ip_tag after every classification.
  // coverage is uint8_t in [0, LEARNING_ROUNDS]; normalise to [0.0, 1.0].
  // We take the max across all valid deltas: a single high-coverage delta is
  // enough to consider this IP as "prefetchable".
  float max_cov = 0.0f;
  for (const auto& d : entry.deltas) {
    if (d.valid) {
      float cov = static_cast<float>(d.coverage) / static_cast<float>(LEARNING_ROUNDS);
      if (cov > max_cov) max_cov = cov;
    }
  }
  coaware::ip_max_coverage[entry.ip_tag] = max_cov;
}

void berti::limit_selected_deltas(ip_delta_entry& entry)
{
  std::vector<delta_entry*> selected;
  selected.reserve(DELTAS_PER_ENTRY);
  for (auto& delta : entry.deltas) {
    if (delta.valid && delta.status != delta_status::none)
      selected.push_back(&delta);
  }

  std::sort(selected.begin(), selected.end(), [](const auto* lhs, const auto* rhs) {
    if (delta_status_priority(lhs->status) != delta_status_priority(rhs->status))
      return delta_status_priority(lhs->status) > delta_status_priority(rhs->status);
    return lhs->coverage > rhs->coverage;
  });

  for (std::size_t index = MAX_SELECTED_DELTAS; index < selected.size(); ++index)
    selected.at(index)->status = delta_status::none;
}

void berti::increment_learning_counter(uint8_t& counter)
{
  if (counter < LEARNING_ROUNDS)
    ++counter;
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

  if (entry.prefetch_valid)
    ++stats.late_prefetches;

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
  ++stats.prefetches_issued;
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
  ++stats.accesses;

  // Co-aware: track demand accesses per (ip_tag, page) this epoch.
  // Keyed on delta_table_ip_tag (10-bit) for consistency with ip_max_coverage.
  // Inner map is bounded at MAX_PAGES_PER_IP; evict the least-accessed entry
  // when full rather than growing unboundedly (Fix M2).
  {
    const uint64_t page_num = addr.to<uint64_t>() >> LOG2_PAGE_SIZE;
    const uint64_t ip_tag   = delta_table_ip_tag(ip);
    auto& page_map = coaware::ip_page_access_count[ip_tag];
    page_map[page_num]++;
    if (page_map.size() > coaware::MAX_PAGES_PER_IP) {
      auto min_it = std::min_element(page_map.begin(), page_map.end(),
          [](const auto& a, const auto& b) { return a.second < b.second; });
      page_map.erase(min_it);
    }
  }

  if (useful_prefetch) {
    ++stats.useful_prefetches;
    const auto latency = consume_prefetch_latency(line);
    train(ip, line, latency, cycle);
  }

  if (!cache_hit || useful_prefetch)
    add_history(ip, line, cycle);

  if (!cache_hit)
    record_demand_issue(line, ip, cycle);

  issue_prefetches(ip, line, cycle);

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
      train(entry->demand_ip, line, latency, cycle);
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
  fmt::print("  ACCESSES:                      {}\n", stats.accesses);
  fmt::print("  DEMAND_ISSUE_RECORDS:          {}\n", stats.demand_issue_records);
  fmt::print("  DEMAND_LATENCY_SAMPLES:        {}\n", stats.demand_latency_samples);
  fmt::print("  DEMAND_LATENCY_CYCLES:         {}\n", stats.demand_latency_cycles);
  fmt::print("  PREFETCHES_ISSUED:             {}\n", stats.prefetches_issued);
  fmt::print("  PREFETCH_ISSUED_L1:            {}\n", stats.prefetch_issued_l1);
  fmt::print("  PREFETCH_ISSUED_L2:            {}\n", stats.prefetch_issued_l2);
  fmt::print("  PREFETCH_LATENCY_SAMPLES:      {}\n", stats.prefetch_latency_samples);
  fmt::print("  PREFETCH_LATENCY_CYCLES:       {}\n", stats.prefetch_latency_cycles);
  fmt::print("  PREFETCHED_LINE_LATENCY_USES:  {}\n", stats.prefetched_line_latency_uses);
  fmt::print("  USEFUL_PREFETCHES:             {}\n", stats.useful_prefetches);
  fmt::print("  LATE_PREFETCHES:               {}\n", stats.late_prefetches);
  fmt::print("  HISTORY_INSERTS:               {}\n", stats.history_inserts);
  fmt::print("  HISTORY_REPLACEMENTS:          {}\n", stats.history_replacements);
  fmt::print("  HISTORY_SEARCHES:              {}\n", stats.history_searches);
  fmt::print("  TIMELY_DELTAS:                 {}\n", stats.timely_deltas);
  fmt::print("  DELTA_TABLE_HITS:              {}\n", stats.delta_table_hits);
  fmt::print("  DELTA_TABLE_MISSES:            {}\n", stats.delta_table_misses);
  fmt::print("  DELTA_TABLE_INSERTS:           {}\n", stats.delta_table_inserts);
  fmt::print("  DELTA_TABLE_REPLACEMENTS:      {}\n", stats.delta_table_replacements);
  fmt::print("  DELTA_INSERTS:                 {}\n", stats.delta_inserts);
  fmt::print("  DELTA_REPLACEMENTS:            {}\n", stats.delta_replacements);
  fmt::print("  DELTA_DISCARDS:                {}\n", stats.delta_discards);
  fmt::print("  TRAINED_DELTAS:                {}\n", stats.trained_deltas);
  fmt::print("  DELTA_CLASSIFICATIONS:         {}\n", stats.delta_classifications);
  fmt::print("  WARMUP_PREFETCH_CANDIDATES:    {}\n", stats.warmup_prefetch_candidates);
  fmt::print("  HISTORY_SKIPPED_MIGRATION:     {}\n", stats.history_skipped_migration);
}
