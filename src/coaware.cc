#include "coaware.h"

namespace coaware {

std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> ip_page_access_count;
std::unordered_map<uint64_t, float>    ip_max_coverage;

std::unordered_map<uint64_t, float>    page_hotness;
std::unordered_map<uint64_t, uint64_t> page_total_accesses;

std::unordered_map<uint64_t, float>    page_uncovered_rate;
std::unordered_map<uint64_t, float>    page_prefetchability;

std::unordered_map<uint64_t, uint64_t> recently_migrated_pages;
std::unordered_map<uint64_t, uint64_t> recently_demoted_pages;

std::unordered_map<uint64_t, int>      high_uncovered_rate_epochs;

uint64_t current_epoch                    = 0;
float    epoch_max_hotness                = 1.0f;
int      cxl_prefetch_budget_remaining    = CXL_PREFETCH_BUDGET_PER_EPOCH;
int      cxl_prefetch_budget_exhausted_epochs = 0;

MEMORY_CONTROLLER* g_mc = nullptr;

} // namespace coaware
