#include "dt_provenance/dt_proxy/proxy_runtime.h"

#include <algorithm>
#include <chrono>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <clio_ctp/serialize/msgpack_wrapper.h>

#include <clio_cte/core/core_client.h>
#include <cstring>

#include "dt_provenance/protocol/anthropic_parser.h"
#include "dt_provenance/protocol/ollama_parser.h"
#include "dt_provenance/protocol/openai_parser.h"
#include "dt_provenance/protocol/cost_estimator.h"
#include "dt_provenance/protocol/interaction.h"
#include "dt_provenance/protocol/provider.h"
#include "dt_provenance/protocol/stream_reassembly.h"
#include "dt_provenance/dt_tracker/tracker_client.h"

namespace dt_provenance::proxy {

using json = nlohmann::ordered_json;
using namespace dt_provenance::protocol;

Runtime::~Runtime() = default;

/**
 * Parse a URL into host, port, and ssl flag
 */
static void ParseUpstreamUrl(const std::string& url, std::string& host,
                             int& port, bool& ssl) {
  if (url.starts_with("https://")) {
    ssl = true;
    host = url.substr(8);
    port = 443;
  } else if (url.starts_with("http://")) {
    ssl = false;
    host = url.substr(7);
    port = 80;
  } else {
    host = url;
    port = 443;
    ssl = true;
  }
  auto colon = host.find(':');
  if (colon != std::string::npos) {
    port = std::stoi(host.substr(colon + 1));
    host = host.substr(0, colon);
  }
  if (!host.empty() && host.back() == '/') host.pop_back();
}

/**
 * Select upstream URL based on provider
 */
static std::string SelectUpstream(Provider provider) {
  switch (provider) {
    case Provider::kAnthropic: return "https://api.anthropic.com";
    case Provider::kOpenAI:    return "https://api.openai.com";
    case Provider::kOllama: {
      // Respect OLLAMA_HOST env var so per-job port assignments work.
      // _aeg_run_common.sh exports OLLAMA_HOST=http://127.0.0.1:<port>.
      const char* env = std::getenv("OLLAMA_HOST");
      return (env && env[0]) ? std::string(env) : "http://localhost:11434";
    }
    default:                   return "";
  }
}

/**
 * Forward request directly to upstream and return response.
 * Now runs on Chimaera worker threads via Monitor handler.
 */
static void ForwardDirect(const std::string& upstream_url,
                          const std::string& path,
                          const std::string& headers_json_str,
                          const std::string& request_body,
                          int& resp_status, std::string& resp_headers_out,
                          std::string& resp_body_out,
                          double& latency_ms) {
  std::string host;
  int port;
  bool ssl;
  ParseUpstreamUrl(upstream_url, host, port, ssl);

  // Build headers from JSON
  json request_headers;
  try {
    request_headers = json::parse(headers_json_str);
  } catch (...) {
    request_headers = json::object();
  }

  httplib::Headers hdr;
  for (auto& [k, v] : request_headers.items()) {
    if (v.is_string()) {
      hdr.emplace(k, v.get<std::string>());
    }
  }

  auto start = std::chrono::steady_clock::now();

  std::string response_body;
  int response_status = 502;
  httplib::Headers response_headers;

  if (ssl) {
    httplib::SSLClient cli(host, port);
    cli.set_connection_timeout(30);
    cli.set_read_timeout(300);
    // DTP_CA_BUNDLE allows custom CA bundles for private/self-signed endpoints.
    const char* ca_bundle = std::getenv("DTP_CA_BUNDLE");
    if (ca_bundle && *ca_bundle) {
      cli.set_ca_cert_path(ca_bundle);
    }
    cli.enable_server_certificate_verification(true);

    auto res = cli.Post(path, hdr, request_body, "application/json");
    if (res) {
      response_status = res->status;
      response_body = res->body;
      response_headers = res->headers;
    }
  } else {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(30);
    cli.set_read_timeout(300);

    auto res = cli.Post(path, hdr, request_body, "application/json");
    if (res) {
      response_status = res->status;
      response_body = res->body;
      response_headers = res->headers;
    }
  }

  auto end = std::chrono::steady_clock::now();
  latency_ms = std::chrono::duration<double, std::milli>(end - start).count();

  // Serialize response headers (normalize keys to lowercase for consistent lookup)
  json resp_hdrs_json = json::object();
  for (const auto& [k, v] : response_headers) {
    std::string lower_k = k;
    std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(), ::tolower);
    resp_hdrs_json[lower_k] = v;
  }

  resp_status = response_status;
  resp_headers_out = resp_hdrs_json.dump();
  resp_body_out = response_body;
}

/**
 * Build an interaction record JSON from request/response data
 */
static std::string BuildInteractionRecord(
    const std::string& session_id, Provider provider,
    const std::string& path, const std::string& headers_json_str,
    const std::string& request_body, int response_status,
    const std::string& resp_headers_str, const std::string& resp_body,
    double latency_ms) {
  InteractionRecord record;
  record.session_id = session_id;
  record.provider = provider;
  record.request.method = "POST";
  record.request.path = path;

  json request_headers;
  try { request_headers = json::parse(headers_json_str); } catch (...) {}
  record.request.headers = request_headers;

  // Parse request body
  try {
    auto req_body = json::parse(request_body);
    if (provider == Provider::kAnthropic) {
      AnthropicParser::ParseRequest(req_body, record);
    } else if (provider == Provider::kOpenAI) {
      OpenAIParser::ParseRequest(req_body, record);
    } else if (provider == Provider::kOllama) {
      OllamaParser::ParseRequest(req_body, path, record);
    }
  } catch (...) {}

  // Parse response
  if (response_status >= 200 && response_status < 300) {
    json resp_hdrs;
    try { resp_hdrs = json::parse(resp_headers_str); } catch (...) {}

    bool is_sse = false;
    if (resp_hdrs.contains("content-type")) {
      std::string ct = resp_hdrs["content-type"].get<std::string>();
      is_sse = ct.find("text/event-stream") != std::string::npos;
    }

    // Ollama streams via NDJSON (application/x-ndjson), not SSE.
    // Also check the request body's stream flag (Ollama defaults to true).
    bool is_ndjson = false;
    if (provider == Provider::kOllama) {
      if (resp_hdrs.contains("content-type")) {
        std::string ct = resp_hdrs["content-type"].get<std::string>();
        is_ndjson = ct.find("application/x-ndjson") != std::string::npos;
      }
      if (!is_ndjson) {
        try {
          auto req_body = json::parse(request_body);
          is_ndjson = req_body.value("stream", true);
        } catch (...) {}
      }
    }

    if (is_sse || is_ndjson) {
      record.response.is_streaming = true;
      if (is_sse) {
        auto chunks = ReassembleSSE(resp_body);
        for (const auto& chunk : chunks) {
          if (provider == Provider::kAnthropic)
            AnthropicParser::ParseStreamChunk(chunk, record);
        }
      } else {
        auto chunks = ReassembleNDJSON(resp_body);
        for (const auto& chunk : chunks) {
          if (provider == Provider::kOllama)
            OllamaParser::ParseStreamChunk(chunk, path, record);
        }
      }
    } else {
      record.response.is_streaming = false;
      try {
        auto body = json::parse(resp_body);
        if (provider == Provider::kAnthropic)
          AnthropicParser::ParseResponse(body, record);
        else if (provider == Provider::kOpenAI)
          OpenAIParser::ParseResponse(body, record);
        else if (provider == Provider::kOllama)
          OllamaParser::ParseResponse(body, path, record);
      } catch (...) {}
    }

    // Estimate cost
    TokenUsage usage;
    usage.input_tokens = record.metrics.input_tokens;
    usage.output_tokens = record.metrics.output_tokens;
    usage.cache_creation_tokens = record.metrics.cache_creation_tokens;
    usage.cache_read_tokens = record.metrics.cache_read_tokens;
    auto cost = CostEstimator::Estimate(provider, record.model, usage);
    record.metrics.cost_usd = cost.total_cost;
  }

  record.metrics.total_latency_ms = latency_ms;
  record.metrics.time_to_first_token_ms = latency_ms;
  record.response.status_code = response_status;

  return record.ToJson().dump();
}

clio::run::TaskResume Runtime::Create(clio::run::shared_ptr<CreateTask> &task) {
  start_time_ = std::chrono::steady_clock::now();
  total_requests_.store(0);

  // Initialize the CTE client with the correct pool ID.
  // The global g_cte_client may be uninitialized (pool_id=0:0) when accessed
  // from a dlopen'd ChiMod .so because the demo server's init doesn't
  // propagate across symbol scopes.
  {
    auto *cte = CLIO_CTE_CLIENT;
    if (cte->pool_id_.IsNull()) {
      clio::run::PoolId cte_pool = CLIO_POOL_MANAGER->FindPoolByName("cte_main");
      if (!cte_pool.IsNull()) {
        cte->Init(cte_pool);
        HLOG(kInfo, "Proxy: initialized CTE client with pool_id={}", cte_pool);
      }
    }
  }

  HLOG(kInfo, "DTProvenance proxy ChiMod loaded (Monitor-based dispatch)");
  (void)task;
  co_return;
}

bool Runtime::EnsureTrackerClient() {
  if (tracker_initialized_) return true;
  clio::run::PoolId pool = CLIO_POOL_MANAGER->FindPoolByName("dt_tracker_pool");
  if (!pool.IsNull()) {
    tracker_client_ = std::make_unique<tracker::Client>(pool);
    tracker_initialized_ = true;
    return true;
  }
  return false;
}

// ── Monitor dispatch table ───────────────────────────────────────────────

clio::run::TaskResume Runtime::Monitor(clio::run::shared_ptr<MonitorTask> &task) {
  const std::string query_str(task->query_);

  if (!query_str.empty() && query_str[0] == '{') {
    co_await HandleForwardAction(std::move(task), query_str);
    co_return;
  }
  if (query_str.starts_with("overhead_logging:")) {
    HandleOverheadLogging(task, query_str);
    task->SetReturnCode(0); co_return;
  }
  if (query_str == "dispatch_stats") {
    HandleDispatchStats(task);
    task->SetReturnCode(0); co_return;
  }
  if (query_str == "list_sessions") {
    co_await HandleListSessions(std::move(task)); co_return;
  }
  if (query_str.rfind("query_session://", 0) == 0) {
    co_await HandleQuerySession(std::move(task), query_str.substr(16)); co_return;
  }
  if (query_str.rfind("get_interaction://", 0) == 0) {
    co_await HandleGetInteraction(std::move(task), query_str.substr(18)); co_return;
  }
  if (query_str == "list_graphs") {
    co_await HandleListGraphs(std::move(task)); co_return;
  }
  if (query_str.rfind("query_graph://", 0) == 0) {
    co_await HandleQueryGraph(std::move(task), query_str.substr(14)); co_return;
  }
  if (query_str.rfind("get_node://", 0) == 0) {
    co_await HandleGetNode(std::move(task), query_str.substr(11)); co_return;
  }
  if (query_str.rfind("store_recovery_event://", 0) == 0) {
    co_await HandleStoreRecoveryEvent(std::move(task), query_str.substr(23)); co_return;
  }
  if (query_str.rfind("query_recovery_events://", 0) == 0) {
    co_await HandleQueryRecoveryEvents(std::move(task), query_str.substr(24)); co_return;
  }
  if (query_str.rfind("ack_recovery_event://", 0) == 0) {
    co_await HandleAckRecoveryEvent(std::move(task), query_str.substr(21)); co_return;
  }
  if (query_str.rfind("store_lg_checkpoint://", 0) == 0) {
    co_await HandleStoreLgCheckpoint(std::move(task), query_str.substr(22)); co_return;
  }
  if (query_str.rfind("query_lg_checkpoints://", 0) == 0) {
    co_await HandleQueryLgCheckpoints(std::move(task), query_str.substr(23)); co_return;
  }
  if (query_str == "list_checkpoint_sessions") {
    co_await HandleListCheckpointSessions(std::move(task)); co_return;
  }
  if (query_str.rfind("store_checkpoint://", 0) == 0) {
    co_await HandleStoreCheckpoint(std::move(task), query_str.substr(19)); co_return;
  }
  if (query_str.rfind("query_checkpoints://", 0) == 0) {
    co_await HandleQueryCheckpoints(std::move(task), query_str.substr(20)); co_return;
  }
  task->SetReturnCode(0);
  co_return;
}

// ── Private Monitor handlers ─────────────────────────────────────────────

void Runtime::HandleOverheadLogging(clio::run::shared_ptr<MonitorTask>& task,
                                    const std::string& query_str) {
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  if (query_str == "overhead_logging:on") {
    overhead_logging_.store(true, std::memory_order_relaxed);
    pk.pack_map(1); pk.pack("enabled"); pk.pack(true);
  } else if (query_str == "overhead_logging:off") {
    overhead_logging_.store(false, std::memory_order_relaxed);
    pk.pack_map(1); pk.pack("enabled"); pk.pack(false);
  } else {
    pk.pack_map(1); pk.pack("enabled");
    pk.pack(overhead_logging_.load(std::memory_order_relaxed));
  }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
}

clio::run::TaskResume Runtime::HandleRecordOnly(clio::run::shared_ptr<MonitorTask> task,
                                           const std::string& query_json) {
  // Called by the Python streaming path after the full response has been
  // consumed — builds the interaction record and stores it via the tracker.
  try {
    auto query = json::parse(query_json);
    std::string session_id = query.value("session_id", "default");
    Provider provider = ProviderFromString(query.value("provider", "anthropic"));
    std::string path = query.value("path", "/v1/messages");
    std::string headers_json = query.contains("headers")
        ? query["headers"].dump() : "{}";
    std::string request_body = query.value("request_body", "");
    int response_status = query.value("response_status", 200);
    std::string response_headers = query.contains("response_headers")
        ? query["response_headers"].dump() : "{}";
    std::string response_body = query.value("response_body", "");
    double latency_ms = query.value("latency_ms", 0.0);

    std::string record_json = BuildInteractionRecord(
        session_id, provider, path, headers_json,
        request_body, response_status, response_headers,
        response_body, latency_ms);

    if (!record_json.empty() && EnsureTrackerClient()) {
      auto store_future = tracker_client_->AsyncStoreInteraction(
          clio::run::PoolQuery::Local(), record_json);
      co_await store_future;
    }
  } catch (const std::exception& e) {
    HLOG(kWarning, "HandleRecordOnly failed: {}", e.what());
  }
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleForwardAction(clio::run::shared_ptr<MonitorTask> task,
                                             const std::string& query_json) {
  try {
    auto query = json::parse(query_json);
    if (query.contains("action") && query["action"] == "record_only") {
      co_await HandleRecordOnly(std::move(task), query_json);
      co_return;
    }
    if (query.contains("action") && query["action"] == "forward") {
      auto forward_future = client_.AsyncForwardHttp(
          clio::run::PoolQuery::Local(), query_json);
      co_await forward_future;
      task->results_[container_id_] =
          std::string(forward_future->response_msgpack_.str());
      std::string record_json(forward_future->record_json_.str());
      if (!record_json.empty() && EnsureTrackerClient()) {
        try {
          const bool logging = overhead_logging_.load(std::memory_order_relaxed);
          auto pipeline_start = logging ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
          auto store_future = tracker_client_->AsyncStoreInteraction(
              clio::run::PoolQuery::Local(), record_json);
          co_await store_future;
          if (logging) {
            auto pipeline_end = std::chrono::steady_clock::now();
            uint64_t pipeline_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    pipeline_end - pipeline_start).count());
            total_pipeline_overhead_us_.fetch_add(pipeline_us,
                                                   std::memory_order_relaxed);
          }
        } catch (...) {
          HLOG(kWarning, "Tracker store failed");
        }
      }
    }
  } catch (const json::parse_error&) {}
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleListSessions(clio::run::shared_ptr<MonitorTask> task) {
  // Fast path: read session IDs from DTP_session_index (O(n), one tag lookup).
  // Each blob name in that tag is a session_id; content is lightweight metadata.
  // Fall back to the old Agentic_session_* scan only when the index is empty
  // (pre-existing deployments before 3b was active).
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  bool used_index = false;
  try {
    const std::string index_tag = "DTP_session_index";
    auto idx_tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(index_tag);
    co_await idx_tag_future;
    auto idx_tag_id = idx_tag_future->tag_id_;
    auto idx_blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(idx_tag_id);
    co_await idx_blobs_future;
    auto& session_blobs = idx_blobs_future->blob_names_;
    if (!session_blobs.empty()) {
      used_index = true;
      pk.pack_array(static_cast<uint32_t>(session_blobs.size()));
      for (const auto& session_id : session_blobs) {
        pk.pack_map(3);
        pk.pack("session_id"); pk.pack(session_id);
        pk.pack("count"); pk.pack(static_cast<uint64_t>(0));
        pk.pack("tag_name"); pk.pack("Agentic_session_" + session_id);
      }
    }
  } catch (...) {}

  if (!used_index) {
    // Fallback: scan Agentic_session_* tags (O(n×m), legacy path).
    try {
      auto future = CLIO_CTE_CLIENT->AsyncTagQuery("Agentic_session_.*", 0);
      co_await future;
      auto tag_names = future->results_;
      const std::string prefix = "Agentic_session_";
      pk.pack_array(static_cast<uint32_t>(tag_names.size()));
      for (const auto& tag_name : tag_names) {
        std::string session_id = tag_name;
        if (session_id.starts_with(prefix))
          session_id = session_id.substr(prefix.size());
        auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
        co_await tag_future;
        auto tag_id = tag_future->tag_id_;
        auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
        co_await blobs_future;
        auto& blobs = blobs_future->blob_names_;
        pk.pack_map(3);
        pk.pack("session_id"); pk.pack(session_id);
        pk.pack("count"); pk.pack(static_cast<uint64_t>(blobs.size()));
        pk.pack("tag_name"); pk.pack(tag_name);
      }
    } catch (...) {
      pk.pack_array(0);
    }
  }

  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleQuerySession(clio::run::shared_ptr<MonitorTask> task,
                                            const std::string& session_id) {
  std::string tag_name = "Agentic_session_" + session_id;
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  try {
    auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
    co_await tag_future;
    auto tag_id = tag_future->tag_id_;
    auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
    co_await blobs_future;
    auto& blobs = blobs_future->blob_names_;
    auto *ipc = CLIO_CPU_IPC;
    pk.pack_array(static_cast<uint32_t>(blobs.size()));
    for (const auto& bname : blobs) {
      auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, bname);
      co_await size_future;
      auto size = size_future->size_;
      if (size == 0) { pk.pack("{}"); continue; }
      auto shm = ipc->AllocateBuffer(size);
      if (shm.IsNull()) { pk.pack("{}"); continue; }
      auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
          tag_id, bname, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
      co_await get_future;
      pk.pack(std::string(reinterpret_cast<char*>(shm.ptr_), size));
      ipc->FreeBuffer(shm);
    }
  } catch (...) {
    pk.pack_array(0);
  }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleGetInteraction(clio::run::shared_ptr<MonitorTask> task,
                                              const std::string& body) {
  auto slash = body.find('/');
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  if (slash != std::string::npos) {
    std::string session_id = body.substr(0, slash);
    uint64_t seq_id = std::stoull(body.substr(slash + 1));
    std::string tag_name = "Agentic_session_" + session_id;
    char blob_buf[16];
    snprintf(blob_buf, sizeof(blob_buf), "%010lu",
             static_cast<unsigned long>(seq_id));
    std::string blob_name(blob_buf);
    try {
      auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
      co_await tag_future;
      auto tag_id = tag_future->tag_id_;
      auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, blob_name);
      co_await size_future;
      auto size = size_future->size_;
      if (size > 0) {
        auto *ipc = CLIO_CPU_IPC;
        auto shm = ipc->AllocateBuffer(size);
        if (!shm.IsNull()) {
          auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
              tag_id, blob_name, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
          co_await get_future;
          pk.pack(std::string(reinterpret_cast<char*>(shm.ptr_), size));
          ipc->FreeBuffer(shm);
        } else { pk.pack("{}"); }
      } else { pk.pack("{}"); }
    } catch (...) { pk.pack("{}"); }
  } else { pk.pack("{}"); }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleListGraphs(clio::run::shared_ptr<MonitorTask> task) {
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  try {
    auto future = CLIO_CTE_CLIENT->AsyncTagQuery("Ctx_graph_.*", 0);
    co_await future;
    auto tag_names = future->results_;
    const std::string prefix = "Ctx_graph_";
    pk.pack_array(static_cast<uint32_t>(tag_names.size()));
    for (const auto& tag_name : tag_names) {
      std::string session_id = tag_name;
      if (session_id.starts_with(prefix))
        session_id = session_id.substr(prefix.size());
      pk.pack(session_id);
    }
  } catch (...) {
    pk.pack_array(0);
  }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleQueryGraph(clio::run::shared_ptr<MonitorTask> task,
                                          const std::string& body) {
  uint64_t since_seq = 0;
  std::string session_id;
  auto qmark = body.find('?');
  if (qmark != std::string::npos) {
    session_id = body.substr(0, qmark);
    std::string params = body.substr(qmark + 1);
    if (params.rfind("since=", 0) == 0) {
      try { since_seq = std::stoull(params.substr(6)); } catch (...) {}
    }
  } else {
    session_id = body;
  }
  std::string graph_tag = "Ctx_graph_" + session_id;
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  try {
    auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(graph_tag);
    co_await tag_future;
    auto tag_id = tag_future->tag_id_;
    auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
    co_await blobs_future;
    auto& blobs = blobs_future->blob_names_;
    std::vector<std::string> filtered;
    for (const auto& bname : blobs) {
      if (since_seq > 0) {
        try { if (std::stoull(bname) <= since_seq) continue; } catch (...) {}
      }
      filtered.push_back(bname);
    }
    auto *ipc = CLIO_CPU_IPC;
    pk.pack_array(static_cast<uint32_t>(filtered.size()));
    for (const auto& bname : filtered) {
      auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, bname);
      co_await size_future;
      auto size = size_future->size_;
      if (size == 0) { pk.pack("{}"); continue; }
      auto shm = ipc->AllocateBuffer(size);
      if (shm.IsNull()) { pk.pack("{}"); continue; }
      auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
          tag_id, bname, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
      co_await get_future;
      pk.pack(std::string(reinterpret_cast<char*>(shm.ptr_), size));
      ipc->FreeBuffer(shm);
    }
  } catch (...) {
    pk.pack_array(0);
  }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleGetNode(clio::run::shared_ptr<MonitorTask> task,
                                       const std::string& body) {
  auto slash = body.find('/');
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  if (slash != std::string::npos) {
    std::string session_id = body.substr(0, slash);
    uint64_t seq_id = std::stoull(body.substr(slash + 1));
    std::string graph_tag = "Ctx_graph_" + session_id;
    char blob_buf[16];
    snprintf(blob_buf, sizeof(blob_buf), "%010lu",
             static_cast<unsigned long>(seq_id));
    std::string blob_name(blob_buf);
    try {
      auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(graph_tag);
      co_await tag_future;
      auto tag_id = tag_future->tag_id_;
      auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, blob_name);
      co_await size_future;
      auto size = size_future->size_;
      if (size > 0) {
        auto *ipc = CLIO_CPU_IPC;
        auto shm = ipc->AllocateBuffer(size);
        if (!shm.IsNull()) {
          auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
              tag_id, blob_name, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
          co_await get_future;
          pk.pack(std::string(reinterpret_cast<char*>(shm.ptr_), size));
          ipc->FreeBuffer(shm);
        } else { pk.pack("{}"); }
      } else { pk.pack("{}"); }
    } catch (...) { pk.pack("{}"); }
  } else { pk.pack("{}"); }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleStoreRecoveryEvent(
    clio::run::shared_ptr<MonitorTask> task, const std::string& json_payload) {
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  try {
    auto payload_json = json::parse(json_payload);
    std::string event_id = payload_json.value("event_id", "");
    std::string target_session_id = payload_json.value("target_session_id", "");
    if (!event_id.empty() && !target_session_id.empty()) {
      std::string tag_name = "Recovery_" + target_session_id;
      auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
      co_await tag_future;
      auto tag_id = tag_future->tag_id_;
      auto *ipc = CLIO_CPU_IPC;
      size_t payload_size = json_payload.size();
      auto shm = ipc->AllocateBuffer(payload_size);
      if (!shm.IsNull()) {
        memcpy(shm.ptr_, json_payload.data(), payload_size);
        auto put_future = CLIO_CTE_CLIENT->AsyncPutBlob(
            tag_id, event_id, 0, payload_size, ctp::ipc::ShmPtr<>(shm.shm_));
        co_await put_future;
        ipc->FreeBuffer(shm);
        pk.pack(event_id);
      } else { pk.pack(std::string("")); }
    } else { pk.pack(std::string("")); }
  } catch (...) { pk.pack(std::string("")); }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleQueryRecoveryEvents(
    clio::run::shared_ptr<MonitorTask> task, const std::string& session_id) {
  std::string tag_name = "Recovery_" + session_id;
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  try {
    auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
    co_await tag_future;
    auto tag_id = tag_future->tag_id_;
    auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
    co_await blobs_future;
    auto& blobs = blobs_future->blob_names_;
    auto *ipc = CLIO_CPU_IPC;
    pk.pack_array(static_cast<uint32_t>(blobs.size()));
    for (const auto& bname : blobs) {
      auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, bname);
      co_await size_future;
      auto size = size_future->size_;
      if (size == 0) {
        pk.pack_array(2); pk.pack(bname); pk.pack(std::string("")); continue;
      }
      auto shm = ipc->AllocateBuffer(size);
      if (shm.IsNull()) {
        pk.pack_array(2); pk.pack(bname); pk.pack(std::string("")); continue;
      }
      auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
          tag_id, bname, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
      co_await get_future;
      pk.pack_array(2); pk.pack(bname);
      pk.pack(std::string(reinterpret_cast<char*>(shm.ptr_), size));
      ipc->FreeBuffer(shm);
    }
  } catch (...) {
    pk.pack_array(0);
  }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleAckRecoveryEvent(
    clio::run::shared_ptr<MonitorTask> task, const std::string& body) {
  auto slash = body.find('/');
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  if (slash != std::string::npos) {
    std::string session_id = body.substr(0, slash);
    std::string blob_name = body.substr(slash + 1);
    std::string tag_name = "Recovery_" + session_id;
    try {
      auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
      co_await tag_future;
      auto tag_id = tag_future->tag_id_;
      auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, blob_name);
      co_await size_future;
      auto size = size_future->size_;
      std::string updated_json;
      if (size > 0) {
        auto *ipc = CLIO_CPU_IPC;
        auto shm = ipc->AllocateBuffer(size);
        if (!shm.IsNull()) {
          auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
              tag_id, blob_name, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
          co_await get_future;
          std::string json_str(reinterpret_cast<char*>(shm.ptr_), size);
          ipc->FreeBuffer(shm);
          try {
            auto evt_json = json::parse(json_str);
            evt_json["acknowledged"] = true;
            updated_json = evt_json.dump();
          } catch (...) { updated_json = json_str; }
        }
      }
      if (!updated_json.empty()) {
        auto *ipc = CLIO_CPU_IPC;
        size_t new_size = updated_json.size();
        auto shm = ipc->AllocateBuffer(new_size);
        if (!shm.IsNull()) {
          memcpy(shm.ptr_, updated_json.data(), new_size);
          auto put_future = CLIO_CTE_CLIENT->AsyncPutBlob(
              tag_id, blob_name, 0, new_size, ctp::ipc::ShmPtr<>(shm.shm_));
          co_await put_future;
          ipc->FreeBuffer(shm);
          pk.pack(std::string("ok"));
        } else { pk.pack(std::string("error")); }
      } else { pk.pack(std::string("error")); }
    } catch (...) { pk.pack(std::string("error")); }
  } else { pk.pack(std::string("error")); }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleStoreLgCheckpoint(
    clio::run::shared_ptr<MonitorTask> task, const std::string& body) {
  auto slash = body.find('/');
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  if (slash != std::string::npos) {
    std::string tag_name = body.substr(0, slash);
    std::string json_payload = body.substr(slash + 1);
    try {
      auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
      co_await tag_future;
      auto tag_id = tag_future->tag_id_;
      auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
      co_await blobs_future;
      auto& blobs = blobs_future->blob_names_;
      uint64_t next_seq = blobs.size() + 1;
      char blob_buf[16];
      snprintf(blob_buf, sizeof(blob_buf), "%010lu",
               static_cast<unsigned long>(next_seq));
      std::string blob_name(blob_buf);
      auto *ipc = CLIO_CPU_IPC;
      size_t payload_size = json_payload.size();
      auto shm = ipc->AllocateBuffer(payload_size);
      if (!shm.IsNull()) {
        memcpy(shm.ptr_, json_payload.data(), payload_size);
        auto put_future = CLIO_CTE_CLIENT->AsyncPutBlob(
            tag_id, blob_name, 0, payload_size, ctp::ipc::ShmPtr<>(shm.shm_));
        co_await put_future;
        ipc->FreeBuffer(shm);
        pk.pack(blob_name);
      } else { pk.pack(std::string("")); }
    } catch (...) { pk.pack(std::string("")); }
  } else { pk.pack(std::string("")); }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleQueryLgCheckpoints(
    clio::run::shared_ptr<MonitorTask> task, const std::string& tag_name) {
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  try {
    auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
    co_await tag_future;
    auto tag_id = tag_future->tag_id_;
    auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
    co_await blobs_future;
    auto& blobs = blobs_future->blob_names_;
    auto *ipc = CLIO_CPU_IPC;
    pk.pack_array(static_cast<uint32_t>(blobs.size()));
    for (const auto& bname : blobs) {
      auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, bname);
      co_await size_future;
      auto size = size_future->size_;
      if (size == 0) {
        pk.pack_array(2); pk.pack(bname); pk.pack(std::string("")); continue;
      }
      auto shm = ipc->AllocateBuffer(size);
      if (shm.IsNull()) {
        pk.pack_array(2); pk.pack(bname); pk.pack(std::string("")); continue;
      }
      auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
          tag_id, bname, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
      co_await get_future;
      pk.pack_array(2); pk.pack(bname);
      pk.pack(std::string(reinterpret_cast<char*>(shm.ptr_), size));
      ipc->FreeBuffer(shm);
    }
  } catch (...) {
    pk.pack_array(0);
  }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleListCheckpointSessions(
    clio::run::shared_ptr<MonitorTask> task) {
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  try {
    auto future = CLIO_CTE_CLIENT->AsyncTagQuery("DTP_checkpoint_.*", 0);
    co_await future;
    auto tag_names = future->results_;
    const std::string prefix = "DTP_checkpoint_";
    pk.pack_array(static_cast<uint32_t>(tag_names.size()));
    for (const auto& tag_name : tag_names) {
      std::string session_id = tag_name;
      if (session_id.starts_with(prefix))
        session_id = session_id.substr(prefix.size());
      pk.pack(session_id);
    }
  } catch (...) {
    pk.pack_array(0);
  }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleStoreCheckpoint(clio::run::shared_ptr<MonitorTask> task,
                                               const std::string& body) {
  auto slash1 = body.find('/');
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  if (slash1 != std::string::npos) {
    std::string session_id = body.substr(0, slash1);
    std::string rest = body.substr(slash1 + 1);
    auto slash2 = rest.find('/');
    if (slash2 != std::string::npos) {
      std::string checkpoint_id = rest.substr(0, slash2);
      std::string json_payload = rest.substr(slash2 + 1);
      std::string tag_name = "DTP_checkpoint_" + session_id;
      try {
        auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
        co_await tag_future;
        auto tag_id = tag_future->tag_id_;
        auto *ipc = CLIO_CPU_IPC;
        size_t payload_size = json_payload.size();
        auto shm = ipc->AllocateBuffer(payload_size);
        if (!shm.IsNull()) {
          memcpy(shm.ptr_, json_payload.data(), payload_size);
          auto put_future = CLIO_CTE_CLIENT->AsyncPutBlob(
              tag_id, checkpoint_id, 0, payload_size,
              ctp::ipc::ShmPtr<>(shm.shm_));
          co_await put_future;
          ipc->FreeBuffer(shm);
          pk.pack(checkpoint_id);
        } else { pk.pack(std::string("")); }
      } catch (...) { pk.pack(std::string("")); }
    } else { pk.pack(std::string("")); }
  } else { pk.pack(std::string("")); }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

clio::run::TaskResume Runtime::HandleQueryCheckpoints(clio::run::shared_ptr<MonitorTask> task,
                                                const std::string& session_id) {
  std::string tag_name = "DTP_checkpoint_" + session_id;
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  try {
    auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
    co_await tag_future;
    auto tag_id = tag_future->tag_id_;
    auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
    co_await blobs_future;
    auto& blobs = blobs_future->blob_names_;
    auto *ipc = CLIO_CPU_IPC;
    pk.pack_array(static_cast<uint32_t>(blobs.size()));
    for (const auto& bname : blobs) {
      auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, bname);
      co_await size_future;
      auto size = size_future->size_;
      if (size == 0) {
        pk.pack_array(2); pk.pack(bname); pk.pack(std::string("")); continue;
      }
      auto shm = ipc->AllocateBuffer(size);
      if (shm.IsNull()) {
        pk.pack_array(2); pk.pack(bname); pk.pack(std::string("")); continue;
      }
      auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
          tag_id, bname, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
      co_await get_future;
      pk.pack_array(2); pk.pack(bname);
      pk.pack(std::string(reinterpret_cast<char*>(shm.ptr_), size));
      ipc->FreeBuffer(shm);
    }
  } catch (...) {
    pk.pack_array(0);
  }
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  task->SetReturnCode(0);
  co_return;
}

// ── ForwardHttp (runs on I/O worker) ────────────────────────────────────

clio::run::TaskResume Runtime::ForwardHttp(clio::run::shared_ptr<ForwardHttpTask> &task) {
  std::string query_json(task->query_json_.str());
  auto query = json::parse(query_json);
  std::string session_id = query.value("session_id", "");
  std::string provider_name = query.value("provider", "");
  std::string path = query.value("path", "/");
  std::string headers_str = query.contains("headers")
      ? query["headers"].dump() : "{}";
  std::string body = query.value("body", "");

  auto provider = ProviderFromString(provider_name);
  std::string upstream_url = SelectUpstream(provider);

  if (upstream_url.empty()) {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);
    pk.pack_map(3);
    pk.pack("status"); pk.pack(502);
    pk.pack("headers"); pk.pack("{}");
    pk.pack("body");
    pk.pack(std::string(R"({"error":"unknown provider: )" +
                        provider_name + R"("})"));
    task->response_msgpack_ = std::string(sbuf.data(), sbuf.size());
    task->record_json_ = "";
    co_return;
  }

  total_requests_.fetch_add(1, std::memory_order_relaxed);

  // Blocking HTTP call — OK, we're on an I/O worker, not Worker 0
  int resp_status = 502;
  std::string resp_headers, resp_body;
  double latency_ms = 0;
  ForwardDirect(upstream_url, path, headers_str, body,
                resp_status, resp_headers, resp_body, latency_ms);

  HLOG(kInfo, "ForwardHttp: session={} provider={} status={} latency={}ms",
       session_id, provider_name, resp_status,
       static_cast<int>(latency_ms));

  // Build interaction record for tracker storage — timed separately from LLM call
  std::string record_json;
  if (resp_status >= 200 && resp_status < 300) {
    try {
      const bool logging = overhead_logging_.load(std::memory_order_relaxed);
      auto record_start = logging ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
      record_json = BuildInteractionRecord(
          session_id, provider, path, headers_str, body,
          resp_status, resp_headers, resp_body, latency_ms);
      if (logging && !record_json.empty()) {
        auto record_end = std::chrono::steady_clock::now();
        double proxy_overhead_ms = std::chrono::duration<double, std::milli>(
            record_end - record_start).count();
        uint64_t proxy_overhead_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                record_end - record_start).count());
        total_proxy_overhead_us_.fetch_add(proxy_overhead_us,
                                           std::memory_order_relaxed);
        // Inject proxy_overhead_ms into the record JSON so it is persisted in CTE
        try {
          auto rec = json::parse(record_json);
          if (rec.contains("metrics")) {
            rec["metrics"]["proxy_overhead_ms"] = proxy_overhead_ms;
          }
          record_json = rec.dump();
        } catch (...) {}
      }
    } catch (...) {
      HLOG(kWarning, "Failed to build interaction record for session={}", session_id);
    }
  }

  // Pack response as msgpack
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  pk.pack_map(3);
  pk.pack("status"); pk.pack(resp_status);
  pk.pack("headers"); pk.pack(resp_headers);
  pk.pack("body"); pk.pack(resp_body);
  task->response_msgpack_ = std::string(sbuf.data(), sbuf.size());
  task->record_json_ = record_json;
  co_return;
}

// ── GetTaskStats ────────────────────────────────────────────────────────

clio::run::TaskStat Runtime::GetTaskStats(const clio::run::Task *task) const {
  // Route ForwardHttp to I/O workers (io_size >= 4096 triggers I/O lane)
  if (task->method_ == Method::kForwardHttp) {
    clio::run::TaskStat stat;
    stat.io_size_ = 8192;
    return stat;
  }
  return clio::run::TaskStat();
}

// ── HandleDispatchStats ─────────────────────────────────────────────────

void Runtime::HandleDispatchStats(clio::run::shared_ptr<MonitorTask>& task) {
  auto now = std::chrono::steady_clock::now();
  auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
      now - start_time_).count();

  // NOTE: Cannot query tracker inline (blocking Wait() would deadlock
  // when called from a Chimaera worker thread via inline Monitor dispatch).
  // active_sessions is reported as 0 for now — the dashboard can query
  // list_sessions separately and count.
  uint64_t active_sessions = 0;
  uint64_t total_requests = total_requests_.load(std::memory_order_relaxed);
  uint64_t proxy_overhead_us = total_proxy_overhead_us_.load(std::memory_order_relaxed);
  uint64_t pipeline_overhead_us = total_pipeline_overhead_us_.load(std::memory_order_relaxed);

  // Average overheads (0 if no requests yet)
  double avg_proxy_overhead_ms = total_requests > 0
      ? static_cast<double>(proxy_overhead_us) / total_requests / 1000.0 : 0.0;
  double avg_pipeline_overhead_ms = total_requests > 0
      ? static_cast<double>(pipeline_overhead_us) / total_requests / 1000.0 : 0.0;

  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(sbuf);
  pk.pack_map(7);
  pk.pack("total_requests"); pk.pack(total_requests);
  pk.pack("active_sessions"); pk.pack(active_sessions);
  pk.pack("uptime_seconds"); pk.pack(static_cast<uint64_t>(uptime_s));
  pk.pack("total_proxy_overhead_us"); pk.pack(proxy_overhead_us);
  pk.pack("total_pipeline_overhead_us"); pk.pack(pipeline_overhead_us);
  pk.pack("avg_proxy_overhead_ms"); pk.pack(avg_proxy_overhead_ms);
  pk.pack("avg_pipeline_overhead_ms"); pk.pack(avg_pipeline_overhead_ms);
  task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
}

// ── Destroy / GetWorkRemaining ──────────────────────────────────────────

clio::run::TaskResume Runtime::Destroy(clio::run::shared_ptr<DestroyTask> &task) {
  HLOG(kInfo, "DTProvenance proxy shutting down");
  tracker_client_.reset();
  tracker_initialized_ = false;
  (void)task;
  co_return;
}

clio::run::u64 Runtime::GetWorkRemaining() const {
  return 0;
}

}  // namespace dt_provenance::proxy

CLIO_TASK_CC(dt_provenance::proxy::Runtime)
