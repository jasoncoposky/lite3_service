#ifndef L3KV_ENGINE_MESH_HPP
#define L3KV_ENGINE_MESH_HPP

#include <boost/asio.hpp>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace l3kv {

using NodeID = uint32_t;

enum class Lane {
  Control,  // Gossip, Cluster Management (Highest Priority)
  Express,  // Metadata, Heartbeat (High Priority, TCP_NODELAY)
  Standard, // Regular KV ops
  Heavy     // Bulk transfer (Low priority)
};

class Mesh {
public:
  using MessageCallback =
      std::function<void(NodeID, Lane, const std::vector<uint8_t> &)>;

  Mesh(boost::asio::io_context &io_context, NodeID my_id, int port);
  ~Mesh();

  // Connect to a peer node
  void connect(NodeID peer_id, const std::string &host, int port);

  // Send payload to peer on specific lane
  // Returns true if queued/sent, false if peer unknown or disconnected
  bool send(NodeID peer_id, Lane lane, std::vector<uint8_t> payload);

  // Register callback for incoming messages
  void set_on_message(MessageCallback cb);

  // Start listening for incoming connections
  void listen();

  std::vector<NodeID> get_active_peers();

  void set_simulated_latency(int ms) { latency_ms_ = ms; }

private:
  class Connection; // Forward declaration of internal connection handling
  struct Peer;

  boost::asio::io_context &io_context_;
  NodeID my_id_;
  int port_;
  boost::asio::ip::tcp::acceptor acceptor_;
  MessageCallback on_message_;

  std::mutex peers_mx_;
  std::map<NodeID, std::shared_ptr<Peer>> peers_;

  void do_accept();

  std::atomic<int> latency_ms_{0}; // Simulated latency
};

} // namespace l3kv

#endif
