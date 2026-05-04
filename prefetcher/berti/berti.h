#ifndef PREFETCHER_BERTI_H
#define PREFETCHER_BERTI_H

#include <cstdint>

#include "access_type.h"
#include "address.h"
#include "modules.h"

class berti : public champsim::modules::prefetcher {
public:
  using prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_final_stats();
};

#endif
