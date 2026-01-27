#include "../src/engine/clock.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <unordered_set>
#include <vector>


using namespace l3kv;

// Helper to hash Timestamp for uniqueness check
struct TimestampHash {
  size_t operator()(const Timestamp &t) const {
    size_t h1 = std::hash<int64_t>{}(t.wall_time);
    size_t h2 = std::hash<uint32_t>{}(t.logical);
    return h1 ^ (h2 << 1);
  }
};

void test_single_thread() {
  std::cout << "TEST: Single Thread Monotonicity..." << std::endl;
  HybridLogicalClock global(1);
  ThreadLocalClock local(&global);

  auto t1 = local.now();
  for (int i = 0; i < 1000; ++i) {
    auto t2 = local.now();
    assert(t2 > t1);
    t1 = t2;
  }
  std::cout << "PASS" << std::endl;
}

void test_batch_efficiency() {
  std::cout << "TEST: Batch Efficiency (Logical Counters)..." << std::endl;
  HybridLogicalClock global(1);
  ThreadLocalClock local(&global);

  // Capture physical time
  auto t1 = local.now();
  auto t2 = local.now();

  // If fast enough, t2 should be t1.wall_time with t2.logical = t1.logical + 1
  // This isn't guaranteed if OS sleeps, but likely.
  if (t1.wall_time == t2.wall_time) {
    assert(t2.logical == t1.logical + 1);
    // It worked.
    std::cout << "PASS (Logical increment witnessed)" << std::endl;
  } else {
    std::cout << "SKIP (Physical time advanced)" << std::endl;
  }
}

void test_multi_thread_uniqueness() {
  std::cout << "TEST: Multi-Thread Uniqueness..." << std::endl;
  HybridLogicalClock global(1);

  const int NUM_THREADS = 10;
  const int OPS_PER_THREAD = 10000;
  std::vector<std::thread> threads;
  std::vector<std::vector<Timestamp>> results(NUM_THREADS);

  std::atomic<bool> start_flag{false};

  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back([&, i]() {
      ThreadLocalClock local(&global);
      while (!start_flag)
        std::this_thread::yield();

      for (int k = 0; k < OPS_PER_THREAD; ++k) {
        results[i].push_back(local.now());
      }
    });
  }

  start_flag = true;
  for (auto &t : threads)
    t.join();

  std::cout << "Verifying uniqueness..." << std::endl;
  std::unordered_set<Timestamp, TimestampHash> all_stamps;
  for (const auto &vec : results) {
    for (const auto &ts : vec) {
      if (all_stamps.count(ts)) {
        std::cerr << "DUPLICATE FOUND: " << ts.wall_time << ":" << ts.logical
                  << std::endl;
        exit(1);
      }
      all_stamps.insert(ts);
    }
  }
  std::cout << "PASS (Collected " << all_stamps.size() << " unique timestamps)"
            << std::endl;
}

int main() {
  test_single_thread();
  test_batch_efficiency();
  test_multi_thread_uniqueness();
  std::cout << "All Clock Tests Passed!" << std::endl;
  return 0;
}
