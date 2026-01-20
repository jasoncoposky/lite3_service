#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>

#include "engine/store.hpp"
#include "http/http_server.hpp"
#include "observability/simple_metrics.hpp"
#include <iostream>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

// Global metrics instance to be accessible from signal handler
SimpleMetrics global_metrics;

struct Config {
  std::string address = "0.0.0.0";
  unsigned short port = 8080;
  int min_threads = 4;
  int max_threads = 16;
  std::string wal_path = "data.wal";
};

// Simple manual JSON parser for flat config
// Avoids heavy dependencies and macro conflicts
std::string extract_string(const std::string &content, const std::string &key) {
  auto pos = content.find("\"" + key + "\"");
  if (pos == std::string::npos)
    return "";
  pos = content.find(":", pos);
  if (pos == std::string::npos)
    return "";
  auto start = content.find("\"", pos + 1);
  if (start == std::string::npos)
    return "";
  auto end = content.find("\"", start + 1);
  if (end == std::string::npos)
    return "";
  return content.substr(start + 1, end - start - 1);
}

int extract_int(const std::string &content, const std::string &key,
                int default_val) {
  auto pos = content.find("\"" + key + "\"");
  if (pos == std::string::npos)
    return default_val;
  pos = content.find(":", pos);
  if (pos == std::string::npos)
    return default_val;
  // Skip whitespace and potentially quote if numbers are quoted
  size_t val_start = pos + 1;
  while (val_start < content.size() &&
         (isspace(content[val_start]) || content[val_start] == '\"'))
    val_start++;
  size_t val_end = val_start;
  while (val_end < content.size() &&
         (isdigit(content[val_end]) || content[val_end] == '-'))
    val_end++;

  if (val_start >= val_end)
    return default_val;
  try {
    return std::stoi(content.substr(val_start, val_end - val_start));
  } catch (...) {
    return default_val;
  }
}

Config load_config(const std::string &path) {
  Config cfg;
  try {
    std::ifstream f(path);
    if (!f.is_open()) {
      std::cerr << "Config file not found: " << path << ". Using defaults."
                << std::endl;
      return cfg;
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    std::string addr = extract_string(content, "address");
    if (!addr.empty())
      cfg.address = addr;

    int port = extract_int(content, "port", -1);
    if (port != -1)
      cfg.port = (unsigned short)port;

    int min_t = extract_int(content, "min_threads", -1);
    if (min_t != -1)
      cfg.min_threads = min_t;

    int max_t = extract_int(content, "max_threads", -1);
    if (max_t != -1)
      cfg.max_threads = max_t;

    std::string wal = extract_string(content, "wal_path");
    if (!wal.empty())
      cfg.wal_path = wal;

    std::cout << "Loaded config from " << path << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "Error loading config: " << e.what() << ". Using defaults."
              << std::endl;
  }
  return cfg;
}

int main(int argc, char *argv[]) {
  try {
    std::string config_path = "config.json";
    if (argc > 1) {
      config_path = argv[1];
    }

    Config cfg = load_config(config_path);

    std::cout << "Starting Lite3 Service..." << std::endl;
    std::cout << "  Address: " << cfg.address << ":" << cfg.port << std::endl;
    std::cout << "  Threads: " << cfg.min_threads << "-" << cfg.max_threads
              << "(Dynamic)" << std::endl;
    std::cout << "  WAL Path: " << cfg.wal_path << std::endl;

    // Register metrics with lite3-cpp
    std::cout << "DEBUG: Metrics init..." << std::endl;
    lite3cpp::set_metrics(&global_metrics);

    // Initialize Database Engine
    std::cout << "DEBUG: Engine init..." << std::endl;
    Engine db(cfg.wal_path);
    std::cout << "DEBUG: Engine init done." << std::endl;

    http_server::http_server server(db, cfg.address, cfg.port, cfg.min_threads,
                                    cfg.max_threads);
    std::cout << "DEBUG: Server init done." << std::endl;

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