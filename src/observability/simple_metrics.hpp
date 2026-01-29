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

  bool record_bytes_received(size_t bytes) override;
  bool record_bytes_sent(size_t bytes) override;
  bool increment_active_connections() override;
  bool decrement_active_connections() override;
  bool record_error(int status_code) override;

  bool increment_sync_ops(std::string_view type) override;
  bool increment_keys_repaired() override;
  bool increment_mesh_bytes(std::string_view lane, size_t bytes,
                            bool is_send) override;
  void set_thread_count(int count);
  int get_active_connections() const { return active_connections_.load(); }

  void dump_metrics() const;
  std::string get_metrics_string() const;
  std::string get_json() const;

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

  std::atomic<size_t> bytes_received_{0};
  std::atomic<size_t> bytes_sent_{0};
  std::atomic<int64_t> active_connections_{0};

  // Minimal error tracking: just a few buckets
  std::atomic<uint64_t> errors_4xx_{0};
  std::atomic<uint64_t> errors_5xx_{0};

  // Sync Stats
  struct SyncStats {
    std::atomic<uint64_t> count{0};
  };
  std::map<std::string, SyncStats> sync_stats_;
  std::atomic<uint64_t> keys_repaired_{0};

  // Mesh Traffic [Lane -> [Sent, Recv]]
  struct LaneStats {
    std::atomic<size_t> sent{0};
    std::atomic<size_t> recv{0};
  };
  // Pre-defined lanes to avoid strings in atomic map?
  // Map with mutex is fine for MVP.
  std::map<std::string, LaneStats> lane_stats_;

  std::atomic<int> thread_count_{0};
};

#endif // SIMPLE_METRICS_HPP
