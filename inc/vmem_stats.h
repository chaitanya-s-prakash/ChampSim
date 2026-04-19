#ifndef VMEM_STATS_H
#define VMEM_STATS_H

#include <cstdint>
#include <string>

struct vmem_stats {
  std::string name = "Virtual Memory";
  uint64_t ddr_capacity_pages = 0;
  uint64_t cxl_capacity_pages = 0;
  uint64_t ddr_page_allocations = 0;
  uint64_t cxl_page_allocations = 0;
  uint64_t current_ddr_pages = 0;
  uint64_t current_cxl_pages = 0;
  uint64_t peak_ddr_pages = 0;
  uint64_t peak_cxl_pages = 0;
};

#endif
