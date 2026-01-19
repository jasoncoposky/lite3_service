#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include "engine/store.hpp"
#include "http/http_server.hpp"
#include "observability/simple_metrics.hpp"
#include <iostream>

// Global metrics instance to be accessible from signal handler
SimpleMetrics global_metrics;

int main() {
  try {
    std::cout << "Starting Lite3 Service..." << std::endl;

    // Register metrics with lite3-cpp
    lite3cpp::set_metrics(&global_metrics);

    // Initialize Database Engine
    Engine db("data.wal");

    std::cout << "Observability enabled. Metrics will be dumped on CTRL+C (or "
                 "SIGTERM)."
              << std::endl;

    http_server::http_server server(db, "0.0.0.0", 8080);

    std::cout << "Lite3 Service listening on :8080" << std::endl;
    server.run();

    // Server has stopped due to signals or explicit stop
    std::cout << "\nServer stopping gracefully..." << std::endl;

    std::cout << "Flushing WAL..." << std::endl;
    db.flush();

    std::cout << "Dumping metrics..." << std::endl;
    global_metrics.dump_metrics();
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}