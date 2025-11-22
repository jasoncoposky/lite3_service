#include "engine/store.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/dispatch.hpp>
#include <iostream>
#include <map>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

Engine db("data.wal");

std::map<std::string, std::string> parse_query(std::string_view query) {
    std::map<std::string, std::string> res;
    size_t pos = 0;
    while (pos < query.size()) {
        size_t eq = query.find('=', pos);
        if (eq == std::string_view::npos) break;
        size_t amp = query.find('&', eq);
        if (amp == std::string_view::npos) amp = query.size();
        std::string k(query.substr(pos, eq - pos));
        std::string v(query.substr(eq + 1, amp - eq - 1));
        res[k] = v;
        pos = amp + 1;
    }
    return res;
}

template<class Body, class Allocator, class Send>
void handle_request(http::request<Body, http::basic_fields<Allocator>>&& req, Send&& send) {
    auto const bad_req = [&](beast::string_view why) {
        http::response<http::string_body> res{http::status::bad_request, req.version()};
        res.set(http::field::server, "Lite3");
        res.body() = std::string(why);
        res.prepare_payload();
        return res;
    };

    std::string target(req.target());
    
    if (req.method() == http::verb::get && target.starts_with("/kv/")) {
        std::string key = target.substr(4);
        auto data = db.get(key);
        if (data.empty()) {
            http::response<http::empty_body> res{http::status::not_found, req.version()};
            return send(std::move(res));
        }
        http::response<http::vector_body<uint8_t>> res{http::status::ok, req.version()};
        res.set(http::field::server, "Lite3");
        res.set(http::field::content_type, "application/octet-stream");
        res.body() = std::move(data);
        res.prepare_payload();
        return send(std::move(res));
    }

    if (req.method() == http::verb::put && target.starts_with("/kv/")) {
        std::string key = target.substr(4);
        db.put(key, req.body());
        http::response<http::empty_body> res{http::status::ok, req.version()};
        return send(std::move(res));
    }

    if (req.method() == http::verb::post && target.starts_with("/kv/")) {
        auto qpos = target.find('?');
        if (qpos == std::string::npos) return send(bad_req("Missing params"));
        std::string key = target.substr(4, qpos - 4);
        auto params = parse_query(target.substr(qpos + 1));

        if (params["op"] == "set_int") {
            int64_t val = std::stoll(params["val"]);
            db.patch_int(key, params["field"], val);
            http::response<http::empty_body> res{http::status::ok, req.version()};
            return send(std::move(res));
        }
        return send(bad_req("Unknown op"));
    }
    return send(bad_req("Unknown method"));
}

void do_session(tcp::socket socket) {
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto req = std::make_shared<http::request<http::string_body>>();
    http::read(socket, *buffer, *req);
    handle_request(std::move(*req), [&](auto&& response) {
        http::write(socket, response);
        socket.shutdown(tcp::socket::shutdown_send);
    });
}

int main() {
    try {
        net::io_context ioc{1};
        tcp::acceptor acceptor{ioc, {tcp::v4(), 8080}};
        std::cout << "Lite3 Service listening on :8080" << std::endl;
        while(true) {
            tcp::socket socket{ioc};
            acceptor.accept(socket);
            std::thread{std::bind(&do_session, std::move(socket))}.detach();
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}
