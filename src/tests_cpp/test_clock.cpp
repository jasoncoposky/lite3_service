#include "../engine/clock.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>


using namespace l3kv;

void test_monotonicity() {
  HybridLogicalClock clock(1);
  Timestamp t1 = clock.now();
  Timestamp t2 = clock.now();

  assert(t2 > t1 && "Clock must be monotonic");
  std::cout << "[PASS] Monotonicity" << std::endl;
}

void test_causality_receive() {
  HybridLogicalClock clock(1);
  Timestamp local = clock.now();

  // Simulate incoming message from future
  Timestamp remote = local;
  remote.wall_time += 1000;
  remote.logical = 0;

  clock.update(remote);
  Timestamp next = clock.now();

  assert(next.wall_time >= remote.wall_time &&
         "Clock must catch up to remote physical time");
  assert(next > remote && "Next event must be after remote event");
  std::cout << "[PASS] Causality Receive" << std::endl;
}

void test_logical_increment() {
  HybridLogicalClock clock(1);

  // Freeze physical time for testing
  // Note: In real impl, we might need dependency injection for time.
  // Here we assume fast loop increments logical

  Timestamp t1 = clock.now();
  Timestamp t2 = clock.now();

  if (t1.wall_time == t2.wall_time) {
    assert(t2.logical > t1.logical &&
           "Logical counter must increment if physical time is same");
  }
  std::cout << "[PASS] Logical Increment" << std::endl;
}

int main() {
  try {
    test_monotonicity();
    test_causality_receive();
    test_logical_increment();
    std::cout << "All Clock Tests Passed!" << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "Test Failed: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
