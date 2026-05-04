#include "berti.h"

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
