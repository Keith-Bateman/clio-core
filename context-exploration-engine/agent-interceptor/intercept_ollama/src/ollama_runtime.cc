#include "dt_provenance/dt_intercept_ollama/ollama_runtime.h"

#include <chrono>
#include <nlohmann/json.hpp>

#include <httplib.h>

#include "dt_provenance/protocol/cost_estimator.h"
#include "dt_provenance/protocol/interaction.h"
#include "dt_provenance/protocol/ollama_parser.h"
#include "dt_provenance/protocol/stream_reassembly.h"
#include "dt_provenance/dt_tracker/tracker_client.h"

namespace dt_provenance::intercept_ollama {

Runtime::~Runtime() = default;

using json = nlohmann::ordered_json;
using namespace dt_provenance::protocol;

/**
 * Parse a URL into host, port, and ssl flag
 */
static void ParseUrl(const std::string& url, std::string& host, int& port,
                     bool& ssl) {
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
    port = 11434;
    ssl = false;
  }

  // Check for port in host
  auto colon = host.find(':');
  if (colon != std::string::npos) {
    port = std::stoi(host.substr(colon + 1));
    host = host.substr(0, colon);
  }

  // Strip trailing slash
  if (!host.empty() && host.back() == '/') {
    host.pop_back();
  }
}

clio::run::TaskResume Runtime::Create(clio::run::shared_ptr<CreateTask> &task) {
  auto params = task->GetParams();
  std::string url(params.upstream_base_url_.str());

  ParseUrl(url, upstream_host_, upstream_port_, upstream_ssl_);

  HLOG(kInfo, "Ollama interception ChiMod created: upstream={}:{} ssl={}",
       upstream_host_, upstream_port_, upstream_ssl_);
  co_return;
}

clio::run::TaskResume Runtime::InterceptAndForward(
    clio::run::shared_ptr<InterceptAndForwardTask> &task) {
  active_requests_.fetch_add(1);
  auto start = std::chrono::steady_clock::now();

  // Extract task fields
  std::string session_id(task->session_id_.str());
  std::string path(task->path_.str());
  std::string headers_json_str(task->headers_json_.str());
  std::string request_body(task->request_body_.str());

  // Parse headers
  json request_headers;
  try {
    request_headers = json::parse(headers_json_str);
  } catch (const json::parse_error&) {
    request_headers = json::object();
  }

  // 1. Create httplib client for this request (Ollama is typically plain HTTP)
  httplib::Headers hdr;
  for (auto& [k, v] : request_headers.items()) {
    if (v.is_string()) {
      hdr.emplace(k, v.get<std::string>());
    }
  }

  std::string response_body;
  int response_status = 502;
  httplib::Headers response_headers;

  // Ollama is almost always plain HTTP (local server)
  httplib::Client cli(upstream_host_, upstream_port_);
  cli.set_connection_timeout(30);
  cli.set_read_timeout(600);  // Local models can be very slow on first load

  auto res = cli.Post(path, hdr, request_body, "application/json");
  if (res) {
    response_status = res->status;
    response_body = res->body;
    response_headers = res->headers;
  }

  auto end = std::chrono::steady_clock::now();
  double latency_ms =
      std::chrono::duration<double, std::milli>(end - start).count();

  // 2. Set OUT fields so the proxy can return the response immediately
  task->response_status_ = response_status;
  task->latency_ms_ = latency_ms;
  task->ttft_ms_ = latency_ms;  // For non-streaming; TODO: measure TTFT for NDJSON

  // Serialize response headers
  json resp_hdrs_json = json::object();
  for (const auto& [k, v] : response_headers) {
    resp_hdrs_json[k] = v;
  }
  task->response_headers_json_ = resp_hdrs_json.dump();
  task->response_body_ = response_body;

  // 3. Parse interaction asynchronously
  if (response_status >= 200 && response_status < 300) {
    InteractionRecord record;
    record.session_id = session_id;
    record.provider = Provider::kOllama;
    record.request.method = "POST";
    record.request.path = path;
    record.request.headers = request_headers;

    // Parse request
    try {
      auto req_body = json::parse(request_body);
      OllamaParser::ParseRequest(req_body, path, record);
    } catch (const json::parse_error&) {
    }

    // Detect streaming response — Ollama uses NDJSON (application/x-ndjson)
    // or plain application/json with newline-delimited objects
    bool is_ndjson = false;
    if (resp_hdrs_json.contains("content-type")) {
      std::string ct = resp_hdrs_json["content-type"].get<std::string>();
      is_ndjson = ct.find("application/x-ndjson") != std::string::npos;
    }
    // Ollama also streams with application/json but multiple JSON objects
    // separated by newlines. Detect by checking if stream was requested.
    if (!is_ndjson) {
      try {
        auto req_body = json::parse(request_body);
        is_ndjson = req_body.value("stream", true);  // Ollama defaults to streaming
      } catch (const json::parse_error&) {
      }
    }

    // Parse response
    if (is_ndjson) {
      record.response.is_streaming = true;
      auto chunks = ReassembleNDJSON(response_body);
      for (const auto& chunk : chunks) {
        OllamaParser::ParseStreamChunk(chunk, path, record);
      }
    } else {
      record.response.is_streaming = false;
      try {
        auto resp_body = json::parse(response_body);
        OllamaParser::ParseResponse(resp_body, path, record);
      } catch (const json::parse_error&) {
      }
    }

    // Estimate cost (always zero for Ollama — local model)
    TokenUsage usage;
    usage.input_tokens = record.metrics.input_tokens;
    usage.output_tokens = record.metrics.output_tokens;
    auto cost =
        CostEstimator::Estimate(Provider::kOllama, record.model, usage);
    record.metrics.cost_usd = cost.total_cost;
    record.metrics.total_latency_ms = latency_ms;
    record.metrics.time_to_first_token_ms = latency_ms;  // TODO

    record.response.status_code = response_status;

    // 4. Dispatch to Tracker
    HLOG(kInfo,
         "Ollama interaction captured: session={} model={} "
         "in_tokens={} out_tokens={} latency={}ms",
         session_id, record.model, record.metrics.input_tokens,
         record.metrics.output_tokens, static_cast<int>(latency_ms));

    if (!tracker_initialized_) {
      clio::run::PoolId pool = CLIO_POOL_MANAGER->FindPoolByName("dt_tracker_pool");
      if (!pool.IsNull()) {
        tracker_client_ = std::make_unique<dt_provenance::tracker::Client>(pool);
        tracker_initialized_ = true;
      }
    }
    if (tracker_initialized_) {
      std::string record_json = record.ToJson().dump();
      auto f = tracker_client_->AsyncStoreInteraction(
          clio::run::PoolQuery::Local(), record_json);
      f.Wait();
    }
  }

  active_requests_.fetch_sub(1);
  co_return;
}

clio::run::TaskResume Runtime::Monitor(clio::run::shared_ptr<MonitorTask> &task) {
  (void)task;
  co_return;
}

clio::run::TaskResume Runtime::Destroy(clio::run::shared_ptr<DestroyTask> &task) {
  HLOG(kInfo, "Ollama interception ChiMod destroyed");
  (void)task;
  co_return;
}

clio::run::u64 Runtime::GetWorkRemaining() const {
  return active_requests_.load();
}

}  // namespace dt_provenance::intercept_ollama

CLIO_TASK_CC(dt_provenance::intercept_ollama::Runtime)
