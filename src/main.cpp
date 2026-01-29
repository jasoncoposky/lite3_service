#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>

#include "engine/mesh.hpp"
#include "engine/store.hpp"
#include "engine/sync_manager.hpp"
#include "http/http_server.hpp"
#include "observability/simple_metrics.hpp"
#include <iostream>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <string_view>

// Global metrics instance to be accessible from signal handler
SimpleMetrics global_metrics;

struct Config {
  std::string address = "0.0.0.0";
  unsigned short port = 8080;
  int min_threads = 4;
  int max_threads = 16;
  std::string wal_path = "data.wal";
  uint32_t node_id = 1;
  int mesh_port = 9090;
  std::string peers = ""; // Format: "id:host:port,id:host:port"
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

    int nid = extract_int(content, "node_id", -1);
    if (nid != -1)
      cfg.node_id = (uint32_t)nid;

    int mp = extract_int(content, "mesh_port", -1);
    if (mp != -1)
      cfg.mesh_port = mp;

    std::string peers = extract_string(content, "peers");
    if (!peers.empty())
      cfg.peers = peers;

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
    std::cout << "  Node ID: " << cfg.node_id << std::endl;
    std::cout << "  Mesh Port: " << cfg.mesh_port << std::endl;

    // Register metrics with lite3-cpp
    lite3cpp::set_metrics(&global_metrics);

    // Initialize Database Engine
    l3kv::Engine db(cfg.wal_path, cfg.node_id);

    // Initialize Mesh and SyncManager (Replication)
    boost::asio::io_context io_context;
    l3kv::Mesh mesh(io_context, cfg.node_id, cfg.mesh_port);
    l3kv::SyncManager sync(mesh, db, cfg.node_id);

    mesh.set_on_message([&](l3kv::NodeID from, l3kv::Lane lane,
                            const std::vector<uint8_t> &payload) {
      if (lane == l3kv::Lane::Control) {
        sync.handle_message(from, payload);
      }
    });

    mesh.listen();
    std::cout << "DEBUG: Mesh listening." << std::endl;

    // Start Mesh IO thread
    std::thread mesh_thread([&io_context]() {
      std::cout << "DEBUG: Mesh thread started." << std::endl;
      try {
        io_context.run();
      } catch (const std::exception &e) {
        std::cerr << "DEBUG: Mesh io_context error: " << e.what() << std::endl;
      }
    });

    // Parse and connect to peers
    if (!cfg.peers.empty()) {
      std::string s = cfg.peers;
      std::cout << "DEBUG: Parsing peers: " << s << std::endl;
      size_t pos = 0;
      while ((pos = s.find_first_not_of(",")) != std::string::npos) {
        s.erase(0, pos);
        size_t end = s.find(",");
        std::string peer_str = s.substr(0, end);
        if (end != std::string::npos)
          s.erase(0, end + 1);
        else
          s.clear();

        // format: id:host:port
        size_t c1 = peer_str.find(":");
        if (c1 != std::string::npos) {
          uint32_t pid = std::stoul(peer_str.substr(0, c1));
          size_t c2 = peer_str.find(":", c1 + 1);
          if (c2 != std::string::npos) {
            std::string host = peer_str.substr(c1 + 1, c2 - c1 - 1);
            int pport = std::stoi(peer_str.substr(c2 + 1));
            std::cout << "Connecting to peer " << pid << " at " << host << ":"
                      << pport << std::endl;
            try {
              mesh.connect(pid, host, pport);
            } catch (const std::exception &e) {
              std::cerr << "Failed to connect to peer " << pid << ": "
                        << e.what() << " (will wait for them to connect to us)"
                        << std::endl;
            }
          }
        }
      }
    }

    std::cout << "DEBUG: Starting SyncManager..." << std::endl;
    sync.start();
    std::cout << "DEBUG: SyncManager started." << std::endl;

    // Start HTTP Server
    std::cout << "DEBUG: Starting HTTP Server..." << std::endl;
    http_server::http_server server(db, cfg.address, cfg.port, cfg.min_threads,
                                    cfg.max_threads);
    std::cout << "Lite3 Service listening on :" << cfg.port << std::endl;
    server.run();

    // Cleanup
    sync.stop();
    io_context.stop();
    if (mesh_thread.joinable())
      mesh_thread.join();

    std::cout << "\nServer stopping gracefully..." << std::endl;
    db.flush();
    global_metrics.dump_metrics();
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}