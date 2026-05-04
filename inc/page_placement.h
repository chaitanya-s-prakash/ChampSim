/*
 * PagePlacementTable — explicit, mutable page-to-tier mapping for M5.
 *
 * Separates two concepts:
 *   logical physical page  — the stable address the CPU and caches use
 *   routed physical page   — the DRAM slot requests are actually sent to
 *
 * Initially they are identical (no redirect).  Migration swaps routed
 * slots between a CXL candidate and a DDR victim without touching any
 * CPU-visible address.
 */

#ifndef PAGE_PLACEMENT_H
#define PAGE_PLACEMENT_H

#include <cstdint>
#include <unordered_map>

#include "address.h"
#include "champsim.h"

class PagePlacementTable
{
public:
  enum class Tier : uint8_t { DDR, CXL };

  struct Entry {
    Tier     current_tier;
    uint64_t routed_ppage;    // page number used for DRAM routing (may differ from logical after migration)

    // MGLRU-like recency: 0 = hottest (recently touched), max = coldest (victim candidate).
    // Only meaningful for DDR-resident pages.
    uint8_t  generation     = 0;

    // Anti-ping-pong: epochs remaining before this page can be migrated again.
    uint32_t cooldown_epochs = 0;
  };

private:
  std::unordered_map<uint64_t, Entry> table_;  // logical page number → Entry

  const uint64_t primary_size_bytes_;          // byte address boundary: [0, primary) = DDR
  const uint64_t ddr_capacity_pages_;
  const uint64_t cxl_capacity_pages_;

  uint64_t ddr_pages_used_ = 0;
  uint64_t cxl_pages_used_ = 0;

  // Determine initial tier purely from address range (pre-migration behavior).
  Tier initial_tier_of(uint64_t logical_ppage) const;

  // Register a page on first access; returns a reference to its entry.
  Entry& register_page(uint64_t logical_ppage);

public:
  PagePlacementTable(uint64_t ddr_capacity_pages, uint64_t cxl_capacity_pages, uint64_t primary_size_bytes);

  // Translate a CPU-visible physical address to the routed DRAM address.
  // Lazily registers the page on first call (same initial tier as address range).
  // The page offset bits are preserved unchanged.
  champsim::address get_routed_address(champsim::address logical_addr);

  // Return the effective tier of a logical address (reflects any migrations).
  // Lazily registers the page on first call.
  Tier get_tier(champsim::address logical_addr);

  // Swap routing between a CXL page and a DDR page.
  // Both pages must already be registered (call get_routed_address/get_tier first).
  void migrate(uint64_t cxl_logical_ppage, uint64_t ddr_logical_ppage);

  // Set how many epochs a page must wait before it can be migrated again.
  // Call for both participants immediately after migrate().
  void set_cooldown(uint64_t logical_ppage, uint32_t epochs);

  // ── MGLRU victim selection ───────────────────────────────────────────────

  // Mark a DDR page as recently accessed (reset generation to 0).
  // Called from MEMORY_CONTROLLER::add_rq for every DDR-bound demand read.
  void touch(uint64_t logical_ppage);

  // Age all DDR pages by one generation (capped at max_generation).
  // Also ticks down cooldown counters for every tracked page.
  // Called by M5Manager every mglru_aging_interval_epochs epochs.
  void age_all_ddr_pages(uint8_t max_generation);

  // Return the logical page number of the best DDR demotion victim:
  // the DDR page in the oldest non-empty generation with no active cooldown.
  // Returns UINT64_MAX if no valid victim exists.
  uint64_t get_ddr_victim(uint8_t max_generation) const;

  // Returns true if the page exists and its cooldown_epochs > 0.
  bool is_in_cooldown(uint64_t logical_ppage) const;

  uint64_t ddr_pages_used()      const { return ddr_pages_used_; }
  uint64_t cxl_pages_used()      const { return cxl_pages_used_; }
  uint64_t ddr_capacity_pages()  const { return ddr_capacity_pages_; }
  uint64_t cxl_capacity_pages()  const { return cxl_capacity_pages_; }
  bool     ddr_has_free_slot()   const { return ddr_pages_used_ < ddr_capacity_pages_; }
};

#endif // PAGE_PLACEMENT_H
