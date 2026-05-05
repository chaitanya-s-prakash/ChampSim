#ifndef ORACLE_FEEDER_H
#define ORACLE_FEEDER_H

#include "address.h"
#include "champsim.h"
#include "modules.h"

struct oracle_feeder : public champsim::modules::prefetcher {
public:
  using champsim::modules::prefetcher::prefetcher;

  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
};

#endif
