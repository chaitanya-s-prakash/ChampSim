/*
 * CmsTracker — Count-Min Sketch with a bounded top-K list.
 *
 * Used for both HPT (keyed on page number) and HWT (keyed on
 * page_num * lines_per_page + line_offset).
 *
 * The CMS gives a bounded over-estimate of each key's access count.
 * The top-K list maintains the current hottest candidates.
 * Both are reset at each migration epoch boundary.
 */

#ifndef CMS_TRACKER_H
#define CMS_TRACKER_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

class CmsTracker
{
public:
  struct Entry {
    uint64_t key   = 0;
    uint32_t count = 0;
  };

private:
  const std::size_t width_;
  const std::size_t depth_;
  const std::size_t top_k_;

  std::vector<std::vector<uint32_t>> table_;  // [depth][width]
  std::vector<Entry>                 topk_;   // at most top_k_ entries

  std::size_t hash_row(uint64_t key, std::size_t row) const;
  void        topk_insert(uint64_t key, uint32_t count);

public:
  CmsTracker(std::size_t width, std::size_t depth, std::size_t top_k);

  // Record one access to key.  Updates CMS and top-K.
  void update(uint64_t key);

  // Return the CMS minimum estimate for key.
  uint32_t estimate(uint64_t key) const;

  // Current top-K entries.  Order is not guaranteed; caller sorts if needed.
  const std::vector<Entry>& top_entries() const { return topk_; }

  // Reset all CMS counters and the top-K list (call at each epoch boundary).
  void reset();

  std::size_t top_k() const { return top_k_; }
};

#endif // CMS_TRACKER_H
