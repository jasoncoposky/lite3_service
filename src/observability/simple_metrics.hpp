#ifndef SIMPLE_METRICS_HPP
#define SIMPLE_METRICS_HPP

#include "observability.hpp"
#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

class SimpleMetrics : public lite3cpp::IMetrics {
public:
  SimpleMetrics() = default;
  ~SimpleMetrics() override = default;

  bool record_latency(std::string_view operation, double seconds) override;
  bool increment_operation_count(std::string_view operation,
                                 std::string_view status) override;
  bool set_buffer_usage(size_t used_bytes) override;
  bool set_buffer_capacity(size_t capacity_bytes) override;
  bool increment_node_splits() override;
  bool increment_hash_collisions() override;

  void dump_metrics() const;
  std::string get_metrics_string() const;

private:
  struct OpStats {
    std::atomic<uint64_t> count{0};
    std::atomic<double> total_latency{0.0};
    std::atomic<double> max_latency{0.0};
  };

  // We use a mutex for the map, but values are atomic.
  // Since operations are limited (get, put, patch), this map won't grow
  // infinitely.
  mutable std::mutex stats_mutex_;
  std::map<std::string, OpStats> operation_stats_;

  std::atomic<size_t> buffer_usage_{0};
  std::atomic<size_t> buffer_capacity_{0};
  std::atomic<uint64_t> node_splits_{0};
  std::atomic<uint64_t> hash_collisions_{0};
};

#endif // SIMPLE_METRICS_HPP
