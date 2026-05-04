#ifndef PREFETCHER_BERTI_H
#define PREFETCHER_BERTI_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "access_type.h"
#include "address.h"
#include "modules.h"

class berti : public champsim::modules::prefetcher {
  enum class delta_status : uint8_t {
    none,
    l2_pref,
    l2_pref_repl,
    l1_pref,
  };

  static constexpr std::size_t HISTORY_SETS = 8;
  static constexpr std::size_t HISTORY_WAYS = 16;
  static constexpr std::size_t HISTORY_IP_TAG_BITS = 7;
  static constexpr std::size_t HISTORY_LINE_BITS = 24;
  static constexpr std::size_t HISTORY_TIMESTAMP_BITS = 16;
  static constexpr std::size_t LATENCY_BITS = 12;

  static constexpr std::size_t DELTA_TABLE_ENTRIES = 16;
  static constexpr std::size_t DELTAS_PER_ENTRY = 16;
  static constexpr std::size_t DELTA_TABLE_IP_TAG_BITS = 10;
  static constexpr std::size_t DELTA_BITS = 13;
  static constexpr std::size_t COVERAGE_COUNTER_BITS = 4;

  static constexpr std::size_t LEARNING_ROUNDS = 16;
  static constexpr std::size_t MAX_TIMELY_DELTAS = 8;
  static constexpr std::size_t MAX_SELECTED_DELTAS = 12;

  static constexpr uint64_t L1_COVERAGE_THRESHOLD = 10;
  static constexpr uint64_t L2_COVERAGE_THRESHOLD = 5;
  static constexpr uint64_t L2_REPLACEABLE_COVERAGE_THRESHOLD = 8;
  static constexpr double L1_MSHR_OCCUPANCY_THRESHOLD = 0.70;

  static constexpr bool ENABLE_WARMUP_PREFETCHING = true;
  static constexpr uint64_t WARMUP_MIN_SEARCHES = 8;
  static constexpr uint64_t WARMUP_COVERAGE_PERCENT = 80;

  static constexpr std::size_t IN_FLIGHT_ENTRIES = 32;
  static constexpr std::size_t PREFETCH_LATENCY_ENTRIES = 768;
  static constexpr uint64_t MAX_STORED_LATENCY = (uint64_t{1} << LATENCY_BITS) - 1;
  static constexpr uint64_t HISTORY_TIMESTAMP_MASK = (uint64_t{1} << HISTORY_TIMESTAMP_BITS) - 1;
  static constexpr uint64_t HISTORY_TIMESTAMP_HALF_RANGE = uint64_t{1} << (HISTORY_TIMESTAMP_BITS - 1);
  static constexpr int64_t MIN_DELTA = -(int64_t{1} << (DELTA_BITS - 1));
  static constexpr int64_t MAX_DELTA = (int64_t{1} << (DELTA_BITS - 1)) - 1;

  struct in_flight_entry {
    bool valid = false;
    uint64_t line = 0;
    bool demand_valid = false;
    champsim::address demand_ip{};
    uint64_t demand_cycle = 0;
    bool prefetch_valid = false;
    uint64_t prefetch_cycle = 0;
  };

  struct prefetch_latency_entry {
    bool valid = false;
    uint64_t line = 0;
    uint64_t latency = 0;
  };

  struct history_entry {
    bool valid = false;
    uint64_t ip_tag = 0;
    uint64_t line = 0;
    uint64_t timestamp = 0;
  };

  struct history_set {
    std::array<history_entry, HISTORY_WAYS> entries{};
    std::size_t next_victim = 0;
  };

  struct delta_entry {
    bool valid = false;
    int64_t delta = 0;
    uint8_t seen_this_round = 0;
    uint8_t coverage = 0;
    delta_status status = delta_status::none;
  };

  struct ip_delta_entry {
    bool valid = false;
    uint64_t ip_tag = 0;
    bool classified = false;
    uint8_t search_count = 0;
    std::array<delta_entry, DELTAS_PER_ENTRY> deltas{};
  };

  struct stats_type {
    uint64_t demand_issue_records = 0;
    uint64_t demand_latency_samples = 0;
    uint64_t demand_latency_cycles = 0;
    uint64_t prefetch_issue_records = 0;
    uint64_t prefetch_issued_l1 = 0;
    uint64_t prefetch_issued_l2 = 0;
    uint64_t prefetch_latency_samples = 0;
    uint64_t prefetch_latency_cycles = 0;
    uint64_t prefetched_line_latency_uses = 0;
    uint64_t history_inserts = 0;
    uint64_t history_replacements = 0;
    uint64_t history_searches = 0;
    uint64_t timely_deltas = 0;
    uint64_t delta_table_inserts = 0;
    uint64_t delta_table_replacements = 0;
    uint64_t delta_inserts = 0;
    uint64_t delta_replacements = 0;
    uint64_t delta_discards = 0;
    uint64_t trained_deltas = 0;
    uint64_t delta_classifications = 0;
    uint64_t warmup_prefetch_candidates = 0;
  };

  std::array<in_flight_entry, IN_FLIGHT_ENTRIES> in_flight{};
  std::array<prefetch_latency_entry, PREFETCH_LATENCY_ENTRIES> prefetch_latencies{};
  std::array<history_set, HISTORY_SETS> history{};
  std::array<ip_delta_entry, DELTA_TABLE_ENTRIES> delta_table{};
  stats_type stats{};
  std::size_t next_in_flight_victim = 0;
  std::size_t next_delta_victim = 0;

  [[nodiscard]] uint64_t current_cycle() const;
  [[nodiscard]] static uint64_t line_number(champsim::address addr);
  [[nodiscard]] static champsim::address address_from_line(uint64_t line);
  [[nodiscard]] static uint64_t ip_hash(champsim::address ip);
  [[nodiscard]] static std::size_t history_set_index(champsim::address ip);
  [[nodiscard]] static uint64_t history_ip_tag(champsim::address ip);
  [[nodiscard]] static uint64_t delta_table_ip_tag(champsim::address ip);
  [[nodiscard]] static uint64_t history_line(uint64_t line);
  [[nodiscard]] static uint64_t history_timestamp(uint64_t cycle);
  [[nodiscard]] static uint64_t history_timestamp_distance(uint64_t older_timestamp, uint64_t newer_cycle);
  [[nodiscard]] static bool history_timestamp_not_after(uint64_t timestamp, uint64_t cycle);
  [[nodiscard]] static uint64_t elapsed_cycles(uint64_t begin, uint64_t end);
  [[nodiscard]] static uint64_t stored_latency(uint64_t latency);
  [[nodiscard]] static bool is_demand(access_type type);

  void add_history(champsim::address ip, uint64_t line, uint64_t cycle);
  [[nodiscard]] std::vector<int64_t> find_timely_deltas(champsim::address ip, uint64_t line, uint64_t latency, uint64_t cycle) const;
  void train(champsim::address ip, uint64_t line, uint64_t latency, uint64_t cycle);
  void issue_prefetches(champsim::address ip, uint64_t line, uint64_t cycle);
  [[nodiscard]] std::vector<delta_entry> selected_deltas(const ip_delta_entry& entry);
  void add_warmup_deltas(const ip_delta_entry& entry, std::vector<delta_entry>& selected);
  [[nodiscard]] static bool warmup_prefetching_ready(const ip_delta_entry& entry);
  [[nodiscard]] static bool selected_delta_status(delta_status status);
  [[nodiscard]] static int delta_status_priority(delta_status status);
  [[nodiscard]] static bool target_line(uint64_t line, int64_t delta, uint64_t& target);

  [[nodiscard]] ip_delta_entry* find_delta_entry(uint64_t ip_tag);
  [[nodiscard]] ip_delta_entry& get_or_allocate_delta_entry(uint64_t ip_tag);
  [[nodiscard]] delta_entry* get_or_allocate_delta(ip_delta_entry& entry, int64_t delta);
  [[nodiscard]] delta_entry* find_delta_replacement(ip_delta_entry& entry);
  [[nodiscard]] static bool replaceable_delta_status(delta_status status);
  void classify(ip_delta_entry& entry);
  void limit_selected_deltas(ip_delta_entry& entry);
  static void increment_learning_counter(uint8_t& counter);

  [[nodiscard]] in_flight_entry* find_in_flight(uint64_t line);
  [[nodiscard]] in_flight_entry& get_or_allocate_in_flight(uint64_t line);
  void record_demand_issue(uint64_t line, champsim::address ip, uint64_t cycle);
  void record_prefetch_issue(uint64_t line, uint64_t cycle);
  void remove_demand_issue(in_flight_entry& entry);
  void remove_prefetch_issue(in_flight_entry& entry);

  [[nodiscard]] static std::size_t prefetch_latency_index(uint64_t line);
  void remember_prefetch_latency(uint64_t line, uint64_t latency);
  void forget_prefetch_latency(uint64_t line);
  uint64_t consume_prefetch_latency(uint64_t line);

public:
  using prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_final_stats();
};

#endif
