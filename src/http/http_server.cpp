#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

// Fix conflict with Boost.Beast
#ifdef off_t
#undef off_t
#endif

#include "http_server.hpp"
#include <boost/asio/dispatch.hpp>
#include <boost/beast/version.hpp>
#include <boost/config.hpp>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/store.hpp"
#include "json.hpp"
#include "observability/simple_metrics.hpp"

namespace http_server {

struct ScopedMetric {
  std::string_view op;
  std::chrono::high_resolution_clock::time_point start;
  ScopedMetric(std::string_view o) : op(o) {
    start = std::chrono::high_resolution_clock::now();
  }
  ~ScopedMetric() {
    try {
      auto end = std::chrono::high_resolution_clock::now();
      double dur = std::chrono::duration<double>(end - start).count();
      // Access global metrics via pointer cast if needed, or just relying on
      // IMetrics interface
      lite3cpp::IMetrics *m =
          lite3cpp::g_metrics.load(std::memory_order_acquire);
      if (m) {
        m->record_latency(op, dur);
      }
    } catch (...) {
    }
  }
};

// Basic session to handle a single request/response
class session : public std::enable_shared_from_this<session> {
  tcp::socket socket_;
  net::io_context &ioc_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  Engine &db_;

public:
  session(tcp::socket &&socket, net::io_context &ioc, Engine &db)
      : socket_(std::move(socket)), ioc_(ioc), db_(db) {
    if (auto *m = lite3cpp::g_metrics.load(std::memory_order_relaxed))
      m->increment_active_connections();
  }

  ~session() {
    if (auto *m = lite3cpp::g_metrics.load(std::memory_order_relaxed))
      m->decrement_active_connections();
  }

  void run() { do_read(); }

private:
  void do_read() {
    req_ = {};
    http::async_read(
        socket_, buffer_, req_,
        beast::bind_front_handler(&session::on_read, shared_from_this()));
  }

  void on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);
    if (auto *m = lite3cpp::g_metrics.load(std::memory_order_relaxed))
      m->record_bytes_received(bytes_transferred);

    if (ec == http::error::end_of_stream) {
      return do_close();
    }
    if (ec) {
      std::cerr << "read: " << ec.message() << "\n";
      return;
    }

    handle_request();
  }

  std::map<std::string, std::string> parse_query(std::string_view query) {
    std::map<std::string, std::string> res;
    size_t pos = 0;
    while (pos < query.size()) {
      size_t eq = query.find('=', pos);
      if (eq == std::string_view::npos)
        break;
      size_t amp = query.find('&', eq);
      if (amp == std::string_view::npos)
        amp = query.size();
      std::string k(query.substr(pos, eq - pos));
      std::string v(query.substr(eq + 1, amp - eq - 1));
      res[k] = v;
      pos = amp + 1;
    }
    return res;
  }

  void handle_request() {
    ScopedMetric sm("handler_total");
    auto const bad_req = [&](beast::string_view why) {
      http::response<http::string_body> res{http::status::bad_request,
                                            req_.version()};
      res.set(http::field::server, "Lite3");
      res.body() = std::string(why);
      res.prepare_payload();
      return res;
    };

#include "dashboard.hpp"

    // ... existing code ...

    std::string target(req_.target());

    if (req_.method() == http::verb::get && target == "/dashboard") {
      http::response<http::string_body> res{http::status::ok, req_.version()};
      res.set(http::field::server, "Lite3");
      res.set(http::field::content_type, "text/html");
      res.body() = std::string(dashboard_html);
      res.prepare_payload();
      return send_response(std::move(res));
    }

    if (req_.method() == http::verb::get && target == "/metrics") {
      http::response<http::string_body> res{http::status::ok, req_.version()};
      res.set(http::field::server, "Lite3");
      res.set(http::field::content_type, "application/json");

      auto *m = lite3cpp::g_metrics.load(std::memory_order_acquire);
      if (auto *sm = dynamic_cast<SimpleMetrics *>(m)) {
        res.body() = sm->get_json();
      } else {
        res.body() = "{}"; // Should not happen
      }

      res.keep_alive(req_.keep_alive());
      res.prepare_payload();
      return send_response(std::move(res));
    }

    if (req_.method() == http::verb::get && target == "/kv/health") {
      http::response<http::empty_body> res{http::status::ok, req_.version()};
      res.set(http::field::server, "Lite3");
      res.keep_alive(req_.keep_alive());
      res.prepare_payload();
      return send_response(std::move(res));
    }

    if (req_.method() == http::verb::get && target == "/kv/metrics") {
      auto *metrics = dynamic_cast<SimpleMetrics *>(lite3cpp::g_metrics.load());
      std::string body;
      if (metrics) {
        body = metrics->get_metrics_string();
      } else {
        body = "Metrics not available (null)\n";
      }

      auto wal_stats = db_.get_wal_stats();
      body += "\n=== WAL Metrics (libconveyor) ===\n";
      body +=
          "Bytes Written: " + std::to_string(wal_stats.bytes_written) + "\n";
      body += "Avg Write Latency: " +
              std::to_string(wal_stats.avg_write_latency.count()) + " ms\n";
      body += "Buffer Full Events: " +
              std::to_string(wal_stats.write_buffer_full_events) + "\n";

      http::response<http::string_body> res{http::status::ok, req_.version()};
      res.set(http::field::server, "Lite3");
      res.body() = std::move(body);
      res.keep_alive(req_.keep_alive());
      res.prepare_payload();
      return send_response(std::move(res));
    }

    if (req_.method() == http::verb::get && target.starts_with("/kv/")) {
      std::string key = target.substr(4);
      lite3cpp::Buffer buffer_data =
          db_.get(key);              // db.get now returns a Buffer
      if (buffer_data.size() == 0) { // Check if buffer is empty
        http::response<http::empty_body> res{http::status::not_found,
                                             req_.version()};
        res.keep_alive(req_.keep_alive());
        return send_response(std::move(res));
      }

      // Convert Buffer to JSON string
      std::string json_string =
          lite3cpp::lite3_json::to_json_string(buffer_data, 0);

      http::response<http::string_body> res{http::status::ok, req_.version()};
      res.set(http::field::server, "Lite3");
      res.set(http::field::content_type,
              "application/json");         // Set content type to JSON
      res.body() = std::move(json_string); // Set body to JSON string
      res.keep_alive(req_.keep_alive());
      res.prepare_payload();
      return send_response(std::move(res));
    }

    if (req_.method() == http::verb::put && target.starts_with("/kv/")) {
      std::string key = target.substr(4);
      try {
        db_.put(key, req_.body());
        http::response<http::empty_body> res{http::status::ok, req_.version()};
        res.keep_alive(req_.keep_alive());
        res.prepare_payload();
        return send_response(std::move(res));
      } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return send_response(bad_req(e.what()));
      }
    }

    if (req_.method() == http::verb::post && target.starts_with("/kv/")) {
      auto qpos = target.find('?');
      if (qpos == std::string::npos)
        return send_response(bad_req("Missing params"));
      std::string key = target.substr(4, qpos - 4);
      auto params = parse_query(target.substr(qpos + 1));

      if (params["op"] == "set_int") {
        int64_t val = std::stoll(params["val"]);
        db_.patch_int(key, params["field"], val);
        http::response<http::empty_body> res{http::status::ok, req_.version()};
        res.keep_alive(req_.keep_alive());
        res.prepare_payload();
        return send_response(std::move(res));
      }
      return send_response(bad_req("Unknown op"));
    }

    if (req_.method() == http::verb::delete_ && target.starts_with("/kv/")) {
      std::string key = target.substr(4);
      if (db_.del(key)) {
        http::response<http::empty_body> res{http::status::ok, req_.version()};
        res.keep_alive(req_.keep_alive());
        res.prepare_payload();
        return send_response(std::move(res));
      } else {
        http::response<http::empty_body> res{http::status::not_found,
                                             req_.version()};
        res.keep_alive(req_.keep_alive());
        res.prepare_payload();
        return send_response(std::move(res));
      }
    }

    return send_response(bad_req("Unknown method"));
  }

  template <class Body, class Allocator>
  void
  send_response(http::response<Body, http::basic_fields<Allocator>> &&res) {
    if (auto *m = lite3cpp::g_metrics.load(std::memory_order_relaxed)) {
      int status = static_cast<int>(res.result());
      m->record_error(status);
    }
    auto sp =
        std::make_shared<http::response<Body, http::basic_fields<Allocator>>>(
            std::move(res));

    http::async_write(socket_, *sp,
                      [self = shared_from_this(), sp](beast::error_code ec,
                                                      std::size_t bytes) {
                        self->on_write(ec, bytes, sp->keep_alive());
                      });
  }

  void on_write(beast::error_code ec, std::size_t bytes_transferred,
                bool keep_alive) {
    boost::ignore_unused(bytes_transferred);
    if (auto *m = lite3cpp::g_metrics.load(std::memory_order_relaxed))
      m->record_bytes_sent(bytes_transferred);

    if (ec) {
      std::cerr << "write: " << ec.message() << "\n";
      return;
    }

    if (keep_alive) {
      do_read();
    } else {
      do_close();
    }
  }

  void do_close() {
    beast::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_send, ec);
  }
};

http_server::http_server(Engine &db, std::string address, unsigned short port)
    : address_(std::move(address)), port_(port), ioc_(1),
      signals_(ioc_, SIGINT, SIGTERM, SIGBREAK),
      acceptor_(ioc_, {net::ip::make_address(address_), port_}), db_(db) {
  signals_.async_wait(
      [this](boost::system::error_code /*ec*/, int /*signal*/) { stop(); });
}

void http_server::run() {
  std::cout << "DEBUG: http_server::run() called" << std::endl;
  do_accept();
  std::cout << "DEBUG: calling ioc_.run()" << std::endl;
  ioc_.run();
  std::cout << "DEBUG: ioc_.run() returned" << std::endl;
}

void http_server::stop() { ioc_.stop(); }

void http_server::do_accept() {
  acceptor_.async_accept(
      net::make_strand(ioc_),
      beast::bind_front_handler(&http_server::on_accept, this));
}

void http_server::on_accept(beast::error_code ec, tcp::socket socket) {
  if (ec) {
    std::cerr << "accept: " << ec.message() << "\n";
    return;
  }

  // Enable TCP_NODELAY to disable Nagle's algorithm
  boost::system::error_code ec_opt;
  socket.set_option(tcp::no_delay(true), ec_opt);
  if (ec_opt) {
    std::cerr << "Failed to set TCP_NODELAY: " << ec_opt.message() << "\n";
  }

  // Create a session and run it
  std::make_shared<session>(std::move(socket), ioc_, db_)->run();

  // Accept another connection
  do_accept();
}

} // namespace http_server
