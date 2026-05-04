#include "berti.h"

#include <fmt/core.h>

void berti::prefetcher_initialize()
{
  fmt::print("Berti prefetcher initialized\n");
}

uint32_t berti::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                         uint32_t metadata_in)
{
  return metadata_in;
}

uint32_t berti::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  return metadata_in;
}

void berti::prefetcher_final_stats()
{
  fmt::print("Berti prefetcher statistics\n");
}
