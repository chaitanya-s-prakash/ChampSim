#include "cms_tracker.h"

#include <algorithm>
#include <cassert>

// splitmix64 finalizer — high-quality, fast, and easy to seed per row.
// Using different row seeds gives the pairwise independence a real CMS needs.
static const uint64_t ROW_SEEDS[] = {
    0x9e3779b97f4a7c15ULL, 0x517cc1b727220a95ULL,
    0xbf58476d1ce4e5b9ULL, 0x94d049bb133111ebULL,
    0x6c62272e07bb0142ULL, 0xad9d7571e84e23c2ULL,
    0x5c4a4a3561d6f2eaULL, 0x9b10f7b08e98c4faULL,
};
static_assert(sizeof(ROW_SEEDS) / sizeof(ROW_SEEDS[0]) >= 8,
              "Need at least 8 row seeds");

CmsTracker::CmsTracker(std::size_t width, std::size_t depth, std::size_t top_k)
    : width_(width), depth_(depth), top_k_(top_k),
      table_(depth, std::vector<uint32_t>(width, 0))
{
  assert(width > 0 && depth > 0 && top_k > 0);
  assert(depth <= sizeof(ROW_SEEDS) / sizeof(ROW_SEEDS[0]));
  topk_.reserve(top_k);
}

std::size_t CmsTracker::hash_row(uint64_t key, std::size_t row) const
{
  uint64_t h = key ^ ROW_SEEDS[row];
  h = ((h >> 30) ^ h) * 0xbf58476d1ce4e5b9ULL;
  h = ((h >> 27) ^ h) * 0x94d049bb133111ebULL;
  h = (h >> 31) ^ h;
  return static_cast<std::size_t>(h % static_cast<uint64_t>(width_));
}

void CmsTracker::update(uint64_t key)
{
  // Step 1: increment CMS counters (saturating at UINT32_MAX).
  for (std::size_t r = 0; r < depth_; ++r) {
    auto& cell = table_[r][hash_row(key, r)];
    if (cell < std::numeric_limits<uint32_t>::max())
      ++cell;
  }

  // Step 2: get the new estimate.
  uint32_t est = estimate(key);

  // Step 3: update the top-K list.
  topk_insert(key, est);
}

uint32_t CmsTracker::estimate(uint64_t key) const
{
  uint32_t min_val = std::numeric_limits<uint32_t>::max();
  for (std::size_t r = 0; r < depth_; ++r)
    min_val = std::min(min_val, table_[r][hash_row(key, r)]);
  return min_val;
}

void CmsTracker::topk_insert(uint64_t key, uint32_t count)
{
  // Check if the key is already tracked.
  auto it = std::find_if(topk_.begin(), topk_.end(),
                         [key](const Entry& e) { return e.key == key; });
  if (it != topk_.end()) {
    it->count = count;
    return;
  }

  // Room in the list — just append.
  if (topk_.size() < top_k_) {
    topk_.push_back({key, count});
    return;
  }

  // List is full — evict the current minimum if our estimate beats it.
  auto min_it = std::min_element(topk_.begin(), topk_.end(),
                                 [](const Entry& a, const Entry& b) {
                                   return a.count < b.count;
                                 });
  if (count > min_it->count)
    *min_it = {key, count};
}

void CmsTracker::reset()
{
  for (auto& row : table_)
    std::fill(row.begin(), row.end(), 0u);
  topk_.clear();
}
