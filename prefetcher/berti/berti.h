#ifndef PREFETCHER_BERTI_H
#define PREFETCHER_BERTI_H

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

  [[nodiscard]] uint64_t current_cycle() const;
  [[nodiscard]] static uint64_t line_number(champsim::address addr);
  [[nodiscard]] static champsim::address address_from_line(uint64_t line);
  [[nodiscard]] static uint64_t ip_hash(champsim::address ip);
  [[nodiscard]] static uint64_t history_ip_tag(champsim::address ip);
  [[nodiscard]] static uint64_t delta_table_ip_tag(champsim::address ip);

public:
  using prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_final_stats();
};

#endif
