#include "http/http_server.hpp"
#include "observability/simple_metrics.hpp"
#include <csignal>
#include <iostream>

// Global metrics instance to be accessible from signal handler
SimpleMetrics global_metrics;

void signal_handler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    std::cout << "\nInterrupt received. Dumping metrics..." << std::endl;
    global_metrics.dump_metrics();
    std::exit(0);
  }
}

int main() {
  try {
    // Register signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Register metrics with lite3-cpp
    lite3cpp::set_metrics(&global_metrics);
    std::cout << "Observability enabled. Metrics will be dumped on CTRL+C."
              << std::endl;

    http_server::http_server server("0.0.0.0", 8080);
    std::cout << "Lite3 Service listening on :8080" << std::endl;
    server.run();
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}