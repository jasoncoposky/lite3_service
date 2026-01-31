#include "clock.hpp"
#include <algorithm> // for std::max
#include <chrono>
#include <iostream>
#include <thread>

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

    // Safety: Check for backwards clock jumps (skew)
    int64_t diff = max_wall_time_ - phys_now;
    if (diff > 5000000) { // 5 seconds
      // Only log periodically to avoid spamming
      static int64_t last_warn = 0;
      if (phys_now - last_warn > 5000000) {
        std::cerr << "[HLC] WARNING: Physical clock lagging logical clock by "
                  << (diff / 1000) << "ms. (System clock moved backwards?)\n";
        last_warn = phys_now;
      }
    }

    // Safety: Overflow protection
    if (max_logical_ == UINT32_MAX) {
      // Critical Error: We exhausted 4 billion logical ticks in 1 microsecond?
      // Or (more likely) the physical clock is stuck/backwards for a LONG time.
      // We MUST wait for physical time to advance to preserve ordering.
      std::cerr << "[HLC] CRITICAL: Logical counter overflow. Blocking until "
                   "physical time advances.\n";
      while (get_physical_time() <= max_wall_time_) {
        std::this_thread::yield();
      }
      auto new_phys = get_physical_time();
      max_wall_time_ = new_phys;
      max_logical_ = 0;
    } else {
      max_logical_++;
    }
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
    return -1;
  }

  // If for_phys_time > max_wall_time_, we advance.
  if (for_phys_time > max_wall_time_) {
    max_wall_time_ = for_phys_time;
    max_logical_ = 0;
  }

  // Overflow check for batch reservation
  if (UINT32_MAX - max_logical_ < (uint32_t)count) {
    return -1; // Cannot reserve
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

    // Reservation failed.
    // Ensure we don't spin too tight if clock is fighting
    std::this_thread::yield();

    int64_t next_phys = get_physical_time();
    if (next_phys == phys_now) {
      // Force fallback to global if stuck
      return global_clock_->now();
    }
    phys_now = next_phys;
  }
}

} // namespace l3kv
