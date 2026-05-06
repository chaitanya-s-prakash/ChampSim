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

  // Reset generation on the newly promoted page so it starts as "hot" in DDR.
  cxl_it->second.generation = 0;
}

bool PagePlacementTable::is_in_cooldown(uint64_t logical_ppage) const
{
  auto it = table_.find(logical_ppage);
  return it != table_.end() && it->second.cooldown_epochs > 0;
}

void PagePlacementTable::set_cooldown(uint64_t logical_ppage, uint32_t epochs)
{
  auto it = table_.find(logical_ppage);
  if (it != table_.end())
    it->second.cooldown_epochs = epochs;
}

void PagePlacementTable::touch(uint64_t logical_ppage)
{
  // Only reset generation if the page is already registered and DDR-resident.
  // Unregistered pages are handled lazily by get_routed_address.
  auto it = table_.find(logical_ppage);
  if (it != table_.end() && it->second.current_tier == Tier::DDR)
    it->second.generation = 0;
}

void PagePlacementTable::age_all_ddr_pages(uint8_t max_generation)
{
  for (auto& [ppage, entry] : table_) {
    if (entry.current_tier == Tier::DDR && entry.generation < max_generation)
      ++entry.generation;
    if (entry.cooldown_epochs > 0)
      --entry.cooldown_epochs;
  }
}

uint64_t PagePlacementTable::get_ddr_victim(uint8_t max_generation) const
{
  // Scan from coldest generation to hottest, return the first eligible DDR page.
  for (int gen = static_cast<int>(max_generation); gen >= 0; --gen) {
    for (const auto& [ppage, entry] : table_) {
      if (entry.current_tier == Tier::DDR
          && entry.generation == static_cast<uint8_t>(gen)
          && entry.cooldown_epochs == 0) {
        return ppage;
      }
    }
  }
  return UINT64_MAX;  // no valid victim found
}
