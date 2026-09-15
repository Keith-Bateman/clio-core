#ifndef DT_PROVENANCE_PROXY_HTTP_PROXY_SERVER_H_
#define DT_PROVENANCE_PROXY_HTTP_PROXY_SERVER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace dt_provenance::proxy {

/**
 * Callback type for forwarding requests to interception ChiMods
 *
 * @param session_id Extracted session ID
 * @param provider_name Detected provider name (e.g., "anthropic")
 * @param method HTTP method (e.g., "POST")
 * @param path Stripped path (e.g., "/v1/messages")
 * @param headers_json JSON-serialized headers
 * @param body Request body
 * @param[out] resp_status Response status code
 * @param[out] resp_headers Response headers JSON
 * @param[out] resp_body Response body
 */
using ForwardCallback = std::function<void(
    const std::string& session_id, const std::string& provider_name,
    const std::string& method, const std::string& path,
    const std::string& headers_json, const std::string& body,
    uint64_t stream_buffer_id,  // 0 = non-streaming
    int& resp_status, std::string& resp_headers, std::string& resp_body)>;

/**
 * HTTP proxy server that listens on a port and routes requests
 *
 * Uses cpp-httplib for the HTTP server. Extracts session IDs from
 * /_session/{id} URL prefixes, detects providers, and dispatches
 * to a callback for forwarding.
 */
class HttpProxyServer {
 public:
  HttpProxyServer();
  ~HttpProxyServer();

  /**
   * Start the proxy server on a background thread
   * @param host Host to bind to (e.g., "0.0.0.0")
   * @param port Port to listen on (e.g., 9090)
   * @param num_threads Number of worker threads
   * @param callback Function to call for each forwarded request
   */
  void Start(const std::string& host, uint16_t port, int num_threads,
             ForwardCallback callback);

  /** Stop the server and join the listener thread */
  void Stop();

  /** Check if the server is running */
  bool IsRunning() const { return running_.load(); }

  /** Get the port the server is listening on */
  uint16_t Port() const { return port_; }

  /** Get count of active sessions seen */
  uint64_t TotalRequests() const { return total_requests_.load(); }

 private:
  std::unique_ptr<httplib::Server> server_;
  std::thread listener_thread_;
  std::atomic<bool> running_{false};
  uint16_t port_ = 0;
  std::atomic<uint64_t> total_requests_{0};
};

}  // namespace dt_provenance::proxy

#endif  // DT_PROVENANCE_PROXY_HTTP_PROXY_SERVER_H_
