#include "dt_provenance/dt_proxy/http_proxy_server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "dt_provenance/protocol/provider.h"
#include "dt_provenance/protocol/session.h"
#include "dt_provenance/dt_proxy/session_guard.h"

namespace dt_provenance::proxy {

using json = nlohmann::ordered_json;

// Hop-by-hop headers to strip from forwarded requests
static const std::vector<std::string> kHopByHopHeaders = {
    "connection",        "keep-alive",       "te",
    "trailers",          "transfer-encoding", "upgrade",
    "host",              "content-length",    "accept-encoding",
    "content-encoding"};

static bool IsHopByHop(const std::string& name) {
  for (const auto& h : kHopByHopHeaders) {
    if (name.size() == h.size()) {
      bool match = true;
      for (size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != h[i]) {
          match = false;
          break;
        }
      }
      if (match) return true;
    }
  }
  return false;
}

HttpProxyServer::HttpProxyServer() : server_(std::make_unique<httplib::Server>()) {}

HttpProxyServer::~HttpProxyServer() { Stop(); }

void HttpProxyServer::Start(const std::string& host, uint16_t port,
                            int num_threads, ForwardCallback callback) {
  if (running_.load()) return;

  port_ = port;

  // Configure server
  server_->new_task_queue = [num_threads] {
    return new httplib::ThreadPool(num_threads);
  };

  // Catch-all handler for ANY path, ANY method
  auto handler = [this, cb = std::move(callback)](const httplib::Request& req,
                                                   httplib::Response& res) {
    total_requests_.fetch_add(1);

    // 1. Try to extract session
    auto session = dt_provenance::protocol::ExtractSession(req.path);

    // 2. Detect provider from path and headers
    std::unordered_map<std::string, std::string> headers;
    for (const auto& [k, v] : req.headers) {
      headers[k] = v;
    }

    std::string effective_path = session ? session->stripped_path : req.path;
    auto provider_info =
        dt_provenance::protocol::DetectProvider(effective_path, headers);

    // 3. If no session, return fake rejection response
    if (!session) {
      auto [content_type, body] =
          BuildSessionRejection(provider_info.provider);
      res.set_content(body, content_type);
      res.status = 200;  // Return 200 so the agent doesn't error
      return;
    }

    // 4. Serialize headers (filtering hop-by-hop)
    json headers_json = json::object();
    for (const auto& [k, v] : req.headers) {
      if (!IsHopByHop(k)) {
        headers_json[k] = v;
      }
    }

    // 5. Dispatch to interception ChiMod via callback
    int resp_status = 502;
    std::string resp_headers_json;
    std::string resp_body;

    cb(session->session_id,
       dt_provenance::protocol::ProviderToString(provider_info.provider),
       req.method, session->stripped_path, headers_json.dump(), req.body,
       0,  // stream_buffer_id = 0 (non-streaming)
       resp_status, resp_headers_json, resp_body);

    // 6. Set response
    res.status = resp_status;

    // Parse and set response headers (strip hop-by-hop and encoding headers
    // since httplib decodes the upstream response and set_content() will
    // set Content-Length)
    if (!resp_headers_json.empty()) {
      try {
        auto resp_hdrs = json::parse(resp_headers_json);
        for (auto& [k, v] : resp_hdrs.items()) {
          if (v.is_string() && !IsHopByHop(k)) {
            res.set_header(k, v.get<std::string>());
          }
        }
      } catch (const json::parse_error&) {
        // Ignore malformed headers
      }
    }

    // Determine content type from response headers or default
    std::string content_type = "application/json";
    if (!resp_headers_json.empty()) {
      try {
        auto resp_hdrs = json::parse(resp_headers_json);
        if (resp_hdrs.contains("content-type")) {
          content_type = resp_hdrs["content-type"].get<std::string>();
        }
      } catch (const json::parse_error&) {}
    }

    res.set_content(resp_body, content_type);
  };

  // Register catch-all for common HTTP methods
  server_->Post(".*", handler);
  server_->Get(".*", handler);
  server_->Put(".*", handler);
  server_->Delete(".*", handler);
  server_->Options(".*", handler);

  // Start listener thread
  running_.store(true);
  listener_thread_ = std::thread([this, host, port]() {
    server_->listen(host, port);
    running_.store(false);
  });
}

void HttpProxyServer::Stop() {
  if (running_.load()) {
    server_->stop();
    if (listener_thread_.joinable()) {
      listener_thread_.join();
    }
    running_.store(false);
  }
}

}  // namespace dt_provenance::proxy
