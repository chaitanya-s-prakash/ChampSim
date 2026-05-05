#ifndef ORACLE_PREFETCH_QUEUE_H
#define ORACLE_PREFETCH_QUEUE_H

#include <cstdint>
#include <deque>

class OraclePrefetchQueue
{
  static constexpr std::size_t MAX_SIZE = 2048;

public:
  static OraclePrefetchQueue& get()
  {
    static OraclePrefetchQueue instance;
    return instance;
  }

  void push(uint64_t vaddr)
  {
    queue_.push_back(vaddr);
    if (queue_.size() > MAX_SIZE)
      queue_.pop_front();
  }

  const std::deque<uint64_t>& queue() const { return queue_; }

  void pop_front() { queue_.pop_front(); }

  bool empty() const { return queue_.empty(); }

  std::size_t size() const { return queue_.size(); }

private:
  OraclePrefetchQueue() = default;
  std::deque<uint64_t> queue_;
};

#endif
