#include "simple_metrics.hpp"
#include <iomanip>
#include <iostream>
#include <sstream>

bool SimpleMetrics::record_latency(std::string_view operation, double seconds) {
  std::string key(operation);
  std::lock_guard<std::mutex> lock(stats_mutex_);
  auto &stats = operation_stats_[key];
  stats.count.fetch_add(1, std::memory_order_relaxed);

  // CAS loop for floating point accumulation if accuracy is critical,
  // but for simple metrics relaxed fetch_add equivalent or simple update is
  // okay-ish actually atomic<double> is not standard until C++20 and might not
  // be supported on all platforms well. simpler approach: just spin lock or use
  // the mutex since this is "simple" metrics. I used std::atomic<double> in
  // header. Let's assume it works (C++20). If not, I'll fall back to mutex
  // logic.

  // Accumulate total latency
  double current = stats.total_latency.load(std::memory_order_relaxed);
  while (!stats.total_latency.compare_exchange_weak(
      current, current + seconds, std::memory_order_relaxed,
      std::memory_order_relaxed)) {
  }

  // Update max latency
  double current_max = stats.max_latency.load(std::memory_order_relaxed);
  while (seconds > current_max &&
         !stats.max_latency.compare_exchange_weak(current_max, seconds,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
  }
  return true;
}

bool SimpleMetrics::increment_operation_count(std::string_view operation,
                                              std::string_view status) {
  std::string key = std::string(operation) + "_" + std::string(status);
  std::lock_guard<std::mutex> lock(stats_mutex_);
  operation_stats_[key].count.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool SimpleMetrics::set_buffer_usage(size_t used_bytes) {
  buffer_usage_.store(used_bytes, std::memory_order_relaxed);
  return true;
}

bool SimpleMetrics::set_buffer_capacity(size_t capacity_bytes) {
  buffer_capacity_.store(capacity_bytes, std::memory_order_relaxed);
  return true;
}

bool SimpleMetrics::increment_node_splits() {
  node_splits_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool SimpleMetrics::increment_hash_collisions() {
  hash_collisions_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void SimpleMetrics::dump_metrics() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  std::cout << "\n=== Internal Service Metrics ===" << std::endl;
  std::cout << "Buffer Usage: " << buffer_usage_.load() << " / "
            << buffer_capacity_.load() << " bytes" << std::endl;
  std::cout << "Node Splits: " << node_splits_.load() << std::endl;
  std::cout << "Hash Collisions: " << hash_collisions_.load() << std::endl;
  std::cout << "Operations:" << std::endl;

  for (const auto &[key, stats] : operation_stats_) {
    uint64_t count = stats.count.load();
    double total = stats.total_latency.load();
    double max_lat = stats.max_latency.load();
    double avg = count > 0 ? total / count : 0.0;

    std::cout << "  " << std::left << std::setw(25) << key
              << " Count: " << std::setw(10) << count
              << " Avg Latency: " << std::fixed << std::setprecision(6) << avg
              << "s"
              << " Max Latency: " << max_lat << "s" << std::endl;
  }
  std::cout << "================================\n" << std::endl;
}

std::string SimpleMetrics::get_metrics_string() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  std::stringstream ss;
  ss << "\n=== Internal Service Metrics ===\n";
  ss << "Buffer Usage: " << buffer_usage_.load() << " / "
     << buffer_capacity_.load() << " bytes\n";
  ss << "Node Splits: " << node_splits_.load() << "\n";
  ss << "Hash Collisions: " << hash_collisions_.load() << "\n";
  ss << "Operations:\n";

  for (const auto &[key, stats] : operation_stats_) {
    uint64_t count = stats.count.load();
    double total = stats.total_latency.load();
    double max_lat = stats.max_latency.load();
    double avg = count > 0 ? total / count : 0.0;

    ss << "  " << std::left << std::setw(25) << key
       << " Count: " << std::setw(10) << count << " Avg Latency: " << std::fixed
       << std::setprecision(6) << avg << "s"
       << " Max Latency: " << max_lat << "s\n";
  }
  ss << "================================\n";
  return ss.str();
}

bool SimpleMetrics::record_bytes_received(size_t bytes) {
  bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
  return true;
}

bool SimpleMetrics::record_bytes_sent(size_t bytes) {
  bytes_sent_.fetch_add(bytes, std::memory_order_relaxed);
  return true;
}

bool SimpleMetrics::increment_active_connections() {
  active_connections_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool SimpleMetrics::decrement_active_connections() {
  active_connections_.fetch_sub(1, std::memory_order_relaxed);
  return true;
}

bool SimpleMetrics::record_error(int status_code) {
  if (status_code >= 500) {
    errors_5xx_.fetch_add(1, std::memory_order_relaxed);
  } else if (status_code >= 400) {
    errors_4xx_.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

std::string SimpleMetrics::get_json() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  std::stringstream ss;
  ss << "{\n";
  ss << "  \"system\": {\n";
  ss << "    \"buffer_usage_bytes\": " << buffer_usage_.load() << ",\n";
  ss << "    \"buffer_capacity_bytes\": " << buffer_capacity_.load() << ",\n";
  ss << "    \"active_connections\": " << active_connections_.load() << ",\n";
  ss << "    \"node_splits\": " << node_splits_.load() << ",\n";
  ss << "    \"hash_collisions\": " << hash_collisions_.load() << "\n";
  ss << "  },\n";
  ss << "  \"throughput\": {\n";
  ss << "    \"bytes_received_total\": " << bytes_received_.load() << ",\n";
  ss << "    \"bytes_sent_total\": " << bytes_sent_.load() << ",\n";
  ss << "    \"http_errors_4xx\": " << errors_4xx_.load() << ",\n";
  ss << "    \"http_errors_5xx\": " << errors_5xx_.load() << "\n";
  ss << "  },\n";
  ss << "  \"operations\": {\n";

  bool first = true;
  for (const auto &[key, stats] : operation_stats_) {
    if (!first)
      ss << ",\n";
    first = false;
    uint64_t count = stats.count.load();
    double total = stats.total_latency.load();
    double max_lat = stats.max_latency.load();
    double avg = count > 0 ? total / count : 0.0;

    ss << "    \"" << key << "\": {\n";
    ss << "      \"count\": " << count << ",\n";
    ss << "      \"avg_latency_s\": " << avg << ",\n";
    ss << "      \"max_latency_s\": " << max_lat << "\n";
    ss << "    }";
  }
  ss << "\n  }\n";
  ss << "}";
  return ss.str();
}
