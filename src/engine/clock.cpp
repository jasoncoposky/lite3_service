#include "clock.hpp"
#include <algorithm> // for std::max
#include <chrono>

namespace l3kv {

// Helper to get physical time (micros)
static int64_t get_physical_time() {
  using namespace std::chrono;
  return duration_cast<microseconds>(system_clock::now().time_since_epoch())
      .count();
}

Timestamp HybridLogicalClock::now() {
  std::lock_guard<std::mutex> lock(mx_);
  auto phys_now = get_physical_time();

  if (phys_now > max_wall_time_) {
    max_wall_time_ = phys_now;
    max_logical_ = 0;
  } else {
    // Clock hasn't moved forward, or we are being called faster than 1us
    // resolution Increment logical counter
    max_logical_++;
  }

  return {max_wall_time_, max_logical_, node_id_};
}

void HybridLogicalClock::update(const Timestamp &incoming) {
  std::lock_guard<std::mutex> lock(mx_);
  auto phys_now = get_physical_time();

  int64_t l_old = max_wall_time_;
  uint32_t c_old = max_logical_;

  int64_t l_msg = incoming.wall_time;
  uint32_t c_msg = incoming.logical;

  // Update local HLC to be max(local, message, physical)
  max_wall_time_ = std::max({l_old, l_msg, phys_now});

  if (max_wall_time_ == l_old && max_wall_time_ == l_msg) {
    max_logical_ = std::max(c_old, c_msg) + 1;
  } else if (max_wall_time_ == l_old) {
    max_logical_ = c_old + 1;
  } else if (max_wall_time_ == l_msg) {
    max_logical_ = c_msg + 1;
  } else {
    max_logical_ = 0;
  }
}

int64_t HybridLogicalClock::reserve_logical(int64_t for_phys_time, int count) {
  std::lock_guard<std::mutex> lock(mx_);
  int64_t phys_now = std::max(get_physical_time(), max_wall_time_);

  if (for_phys_time < phys_now) {
    // Caller's time is stale. They should update their physical time and retry.
    // However, if we forced clock forward (debug), max_wall_time_ >
    // get_physical_time().
    return -1;
  }

  // If for_phys_time > max_wall_time_, we advance.
  if (for_phys_time > max_wall_time_) {
    max_wall_time_ = for_phys_time;
    max_logical_ = 0;
  }

  int64_t start_logical = max_logical_ + 1;
  max_logical_ += count;
  return start_logical;
}

Timestamp ThreadLocalClock::now() {
  int64_t phys_now = get_physical_time();

  // 1. Try to use batch
  if (phys_now == cached_phys_time_) {
    if (cached_logical_next_ < cached_logical_end_) {
      return {cached_phys_time_, cached_logical_next_++,
              global_clock_->get_node_id()};
    }
  } else if (phys_now > cached_phys_time_) {
    // Time moved forward, reset batch
    cached_phys_time_ = phys_now;
    cached_logical_next_ = 0;
    cached_logical_end_ = 0;
  }

  // 2. Refill batch
  // Optimization: If we are here, either time advanced (and we have no batch)
  // or time didn't advance (and batch exhausted).
  // We need to reserve new batch from global clock.
  const int BATCH_SIZE = 50;
  while (true) {
    // Attempt reservation
    int64_t start_logical =
        global_clock_->reserve_logical(phys_now, BATCH_SIZE);
    if (start_logical >= 0) {
      cached_phys_time_ = phys_now; // Confirmed by global clock
      cached_logical_next_ = static_cast<uint32_t>(start_logical);
      cached_logical_end_ = cached_logical_next_ + BATCH_SIZE;
      return {cached_phys_time_, cached_logical_next_++,
              global_clock_->get_node_id()};
    }

    // Reservation failed (global clock is ahead). Update local time and retry.
    // Actually, if global clock is ahead (e.g. from receive update), we should
    // catch up.
    // reserve_logical returns -1 if for_phys_time < global max.
    // We should peek global max? Or simply rely on reserve_logical logic?
    // Let's grab physically again.
    int64_t next_phys = get_physical_time();
    if (next_phys == phys_now) {
      // Physical time hasn't changed, but global is ahead?
      // This implies receive update pushed global forward.
      // We must increment physical time to at least global?
      // Wait, we can't just invent physical time unless we track max locally OR
      // query global.
      // Better strategy: ask global for "current or newer".
      // Simplified: Just use Global Now fallback for single tick if we are
      // fighting? No, we want batching.

      // Let's assume we simply retry with new physical time.
      // If we are spinning, eventually physical time updates.
      // But if clock skew, we might spin for ms.
      // Better: Global clock shoud just "Adopt" our for_phys_time if valid?
      // No...
      // Real implementation: We need to know what 'time' to ask for.
      // If global is ahead, we should ask for global's time?
      Timestamp global_now = global_clock_->now(); // This ticks logical!
      // This defeats batching if we use it.
      // But it gives us valid time.
      return global_now;
    }
    phys_now = next_phys;
  }
}

} // namespace l3kv
