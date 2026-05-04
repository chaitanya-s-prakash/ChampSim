#ifndef PREFETCHER_BERTI_H
#define PREFETCHER_BERTI_H

#include <array>
#include <cstddef>
#include <cstdint>

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

  static constexpr uint64_t WARMUP_MIN_SEARCHES = 8;
  static constexpr uint64_t WARMUP_COVERAGE_PERCENT = 80;

  static constexpr std::size_t IN_FLIGHT_ENTRIES = 32;
  static constexpr std::size_t PREFETCH_LATENCY_ENTRIES = 768;
  static constexpr uint64_t MAX_STORED_LATENCY = (uint64_t{1} << LATENCY_BITS) - 1;

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

  struct latency_stats {
    uint64_t demand_issue_records = 0;
    uint64_t demand_latency_samples = 0;
    uint64_t demand_latency_cycles = 0;
    uint64_t prefetch_issue_records = 0;
    uint64_t prefetch_latency_samples = 0;
    uint64_t prefetch_latency_cycles = 0;
    uint64_t prefetched_line_latency_uses = 0;
  };

  std::array<in_flight_entry, IN_FLIGHT_ENTRIES> in_flight{};
  std::array<prefetch_latency_entry, PREFETCH_LATENCY_ENTRIES> prefetch_latencies{};
  latency_stats stats{};
  std::size_t next_in_flight_victim = 0;

  [[nodiscard]] uint64_t current_cycle() const;
  [[nodiscard]] static uint64_t line_number(champsim::address addr);
  [[nodiscard]] static champsim::address address_from_line(uint64_t line);
  [[nodiscard]] static uint64_t ip_hash(champsim::address ip);
  [[nodiscard]] static uint64_t history_ip_tag(champsim::address ip);
  [[nodiscard]] static uint64_t delta_table_ip_tag(champsim::address ip);
  [[nodiscard]] static uint64_t elapsed_cycles(uint64_t begin, uint64_t end);
  [[nodiscard]] static uint64_t stored_latency(uint64_t latency);
  [[nodiscard]] static bool is_demand(access_type type);

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
