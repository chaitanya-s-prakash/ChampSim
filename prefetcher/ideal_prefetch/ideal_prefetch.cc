#include "ideal_prefetch.h"

#include <fmt/core.h>

#include "cache.h"
#include "oracle_prefetch_queue.h"

// How many future addresses to prefetch per LLC demand access.
static constexpr std::size_t LOOKAHEAD = 16;

void ideal_prefetch::prefetcher_initialize()
{
  pf_issued = 0;
  pf_useful = 0;
}

uint32_t ideal_prefetch::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                                  uint32_t metadata_in)
{
  if (useful_prefetch)
    pf_useful++;

  auto& oracle = OraclePrefetchQueue::get();
  const auto& q = oracle.queue();

  // Read from the BACK of the queue: the most recently pushed entries are the
  // most future addresses (pushed by L2C ahead of LLC's current position).
  // The front holds stale/warmup entries already processed by LLC.
  if (q.size() < LOOKAHEAD)
    return metadata_in;

  auto it = q.cend();
  std::advance(it, -static_cast<std::ptrdiff_t>(LOOKAHEAD));

  for (; it != q.cend(); ++it) {
    // Align to cache block boundary
    champsim::address pf_addr{champsim::block_number{champsim::address{*it}}};

    // Issue prefetch regardless of whether data is in DDR or CXL.
    // prefetch_line() routes the request down the hierarchy to whichever
    // memory tier currently holds the page - no tier filtering needed.
    bool success = prefetch_line(pf_addr, true, metadata_in);
    if (success)
      pf_issued++;
  }

  return metadata_in;
}

uint32_t ideal_prefetch::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                               uint32_t metadata_in)
{
  return metadata_in;
}

void ideal_prefetch::prefetcher_final_stats()
{
  fmt::print("=== Ideal Prefetcher Stats ===\n");
  fmt::print("  Prefetches issued : {}\n", pf_issued);
  fmt::print("  Useful prefetches : {}\n", pf_useful);
  if (pf_issued > 0)
    fmt::print("  Accuracy          : {:.1f}%\n", 100.0 * pf_useful / pf_issued);
}
