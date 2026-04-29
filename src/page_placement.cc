#include "page_placement.h"

#include <cassert>

PagePlacementTable::PagePlacementTable(uint64_t ddr_capacity_pages, uint64_t cxl_capacity_pages, uint64_t primary_size_bytes)
    : primary_size_bytes_(primary_size_bytes), ddr_capacity_pages_(ddr_capacity_pages), cxl_capacity_pages_(cxl_capacity_pages)
{
}

PagePlacementTable::Tier PagePlacementTable::initial_tier_of(uint64_t logical_ppage) const
{
  // A page is initially DDR if its byte address falls below the primary (DDR) size.
  // This matches VirtualMemory's free-list ordering: DDR pages have low addresses,
  // CXL pages have addresses >= primary_size_bytes.
  uint64_t byte_addr = logical_ppage << LOG2_PAGE_SIZE;
  return (byte_addr < primary_size_bytes_) ? Tier::DDR : Tier::CXL;
}

PagePlacementTable::Entry& PagePlacementTable::register_page(uint64_t logical_ppage)
{
  auto [it, inserted] = table_.try_emplace(logical_ppage, Entry{initial_tier_of(logical_ppage), logical_ppage});
  if (inserted) {
    if (it->second.current_tier == Tier::DDR)
      ++ddr_pages_used_;
    else
      ++cxl_pages_used_;
  }
  return it->second;
}

champsim::address PagePlacementTable::get_routed_address(champsim::address logical_addr)
{
  uint64_t raw         = logical_addr.to<uint64_t>();
  uint64_t ppage_num   = raw >> LOG2_PAGE_SIZE;
  uint64_t page_off    = raw & ((1ULL << LOG2_PAGE_SIZE) - 1);

  const Entry& e = register_page(ppage_num);

  uint64_t routed_raw = (e.routed_ppage << LOG2_PAGE_SIZE) | page_off;
  return champsim::address{routed_raw};
}

PagePlacementTable::Tier PagePlacementTable::get_tier(champsim::address logical_addr)
{
  uint64_t ppage_num = logical_addr.to<uint64_t>() >> LOG2_PAGE_SIZE;
  return register_page(ppage_num).current_tier;
}

void PagePlacementTable::migrate(uint64_t cxl_logical_ppage, uint64_t ddr_logical_ppage)
{
  auto cxl_it = table_.find(cxl_logical_ppage);
  auto ddr_it = table_.find(ddr_logical_ppage);

  assert(cxl_it != table_.end() && "CXL page not registered before migration");
  assert(ddr_it != table_.end() && "DDR page not registered before migration");
  assert(cxl_it->second.current_tier == Tier::CXL && "Expected CXL page for promotion");
  assert(ddr_it->second.current_tier == Tier::DDR && "Expected DDR page for demotion");

  // Swap the DRAM slots the two pages are routed to.
  std::swap(cxl_it->second.routed_ppage, ddr_it->second.routed_ppage);

  // Update tier labels.
  cxl_it->second.current_tier = Tier::DDR;  // promoted
  ddr_it->second.current_tier = Tier::CXL;  // demoted

  // Aggregate page counts are unchanged: DDR still holds the same number of
  // pages overall; we just swapped which logical pages occupy those slots.
}
