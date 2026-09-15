#include "dt_provenance/dt_ctx_untangler/ctx_untangler_runtime.h"

#include <clio_cte/core/core_client.h>
#include <clio_ctp/serialize/msgpack_wrapper.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>

namespace dt_provenance::ctx_untangler {

using json = nlohmann::ordered_json;

clio::run::TaskStat Runtime::GetTaskStats(const clio::run::Task *task) const {
  // ComputeDiff performs CTE GetBlob + PutBlob I/O — report io_size >= 4096
  // so the DefaultScheduler routes it to an I/O worker instead of worker 0.
  if (task->method_ == Method::kComputeDiff) {
    clio::run::TaskStat stat;
    stat.io_size_ = 8192;
    return stat;
  }
  return clio::run::TaskStat();
}

std::string Runtime::FormatBlobName(uint64_t seq_id) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%010lu", static_cast<unsigned long>(seq_id));
  return std::string(buf);
}

std::string Runtime::BuildGraphTagName(const std::string& session_id) {
  return "Ctx_graph_" + session_id;
}

std::string Runtime::BuildInteractionTagName(const std::string& session_id) {
  return "Agentic_session_" + session_id;
}

clio::run::TaskResume Runtime::Create(clio::run::shared_ptr<CreateTask> &task) {
  // Initialize the CTE client with the correct pool ID if needed.
  {
    auto *cte = CLIO_CTE_CLIENT;
    if (cte->pool_id_.IsNull()) {
      clio::run::PoolId cte_pool = CLIO_POOL_MANAGER->FindPoolByName("cte_main");
      if (!cte_pool.IsNull()) {
        cte->Init(cte_pool);
        HLOG(kInfo, "Untangler: initialized CTE client with pool_id={}", cte_pool);
      }
    }
  }

  HLOG(kInfo, "Context Untangler ChiMod created");
  co_return;
}

clio::run::TaskResume Runtime::ComputeDiff(clio::run::shared_ptr<ComputeDiffTask> &task) {
  const bool logging = overhead_logging_.load(std::memory_order_relaxed);
  auto diff_start = logging ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};

  std::string session_id(task->session_id_.str());
  uint64_t seq_id = task->sequence_id_;

  // 1. Read the new interaction from CTE
  std::string interaction_tag_name = BuildInteractionTagName(session_id);
  std::string blob_name = FormatBlobName(seq_id);

  json interaction;
  try {
    auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(interaction_tag_name);
    co_await tag_future;
    auto tag_id = tag_future->tag_id_;

    auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, blob_name);
    co_await size_future;
    auto size = size_future->size_;
    if (size == 0) {
      HLOG(kError, "ComputeDiff: blob {} not found in {}", blob_name,
           interaction_tag_name);
      task->success_ = 0;
      co_return;
    }

    auto *ipc = CLIO_CPU_IPC;
    auto shm = ipc->AllocateBuffer(size);
    if (shm.IsNull()) {
      HLOG(kError, "ComputeDiff: AllocateBuffer({}) failed for interaction", size);
      task->success_ = 0;
      co_return;
    }
    auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
        tag_id, blob_name, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
    co_await get_future;
    interaction = json::parse(
        std::string(reinterpret_cast<char*>(shm.ptr_), size));
    ipc->FreeBuffer(shm);
  } catch (const std::exception& e) {
    HLOG(kError, "ComputeDiff: failed to read interaction: {}", e.what());
    task->success_ = 0;
    co_return;
  }

  // 2. Read the previous interaction (if any) using parent_sequence_id
  json prev_interaction;
  uint64_t parent_seq_id = 0;
  if (interaction.contains("conversation") &&
      interaction["conversation"].contains("parent_sequence_id")) {
    parent_seq_id = interaction["conversation"]["parent_sequence_id"].get<uint64_t>();
  }

  if (parent_seq_id > 0) {
    try {
      auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(interaction_tag_name);
      co_await tag_future;
      auto tag_id = tag_future->tag_id_;

      std::string prev_blob = FormatBlobName(parent_seq_id);
      auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, prev_blob);
      co_await size_future;
      auto size = size_future->size_;
      if (size > 0) {
        auto *ipc = CLIO_CPU_IPC;
        auto shm = ipc->AllocateBuffer(size);
        if (!shm.IsNull()) {
        auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
            tag_id, prev_blob, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
        co_await get_future;
        prev_interaction = json::parse(
            std::string(reinterpret_cast<char*>(shm.ptr_), size));
        ipc->FreeBuffer(shm);
        }
      }
    } catch (...) {}
  }

  // 3. Extract metrics from current interaction
  int64_t input_tokens = 0, output_tokens = 0;
  int64_t cache_read_tokens = 0, cache_creation_tokens = 0;
  double cost_usd = 0.0, latency_ms = 0.0, ttft_ms = 0.0;
  if (interaction.contains("metrics")) {
    const auto& m = interaction["metrics"];
    input_tokens = m.value("input_tokens", (int64_t)0);
    output_tokens = m.value("output_tokens", (int64_t)0);
    cache_read_tokens = m.value("cache_read_tokens", (int64_t)0);
    cache_creation_tokens = m.value("cache_creation_tokens", (int64_t)0);
    cost_usd = m.value("cost_usd", 0.0);
    latency_ms = m.value("total_latency_ms", 0.0);
    ttft_ms = m.value("time_to_first_token_ms", 0.0);
  }
  // Effective context window = all tokens sent to the model, cached or not
  int64_t effective_input_tokens = input_tokens + cache_read_tokens + cache_creation_tokens;

  // Count messages in request body
  int64_t message_count = 0;
  if (interaction.contains("request") &&
      interaction["request"].contains("body") &&
      interaction["request"]["body"].contains("messages")) {
    message_count = static_cast<int64_t>(
        interaction["request"]["body"]["messages"].size());
  }

  // Tool call count
  int64_t tool_call_count = 0;
  if (interaction.contains("response") &&
      interaction["response"].contains("tool_calls")) {
    tool_call_count = static_cast<int64_t>(
        interaction["response"]["tool_calls"].size());
  }

  // 4. Extract metrics from previous interaction (for deltas)
  int64_t prev_input_tokens = 0, prev_output_tokens = 0;
  int64_t prev_cache_read_tokens = 0, prev_cache_creation_tokens = 0;
  double prev_cost_usd = 0.0;
  int64_t prev_message_count = 0;
  if (!prev_interaction.is_null()) {
    if (prev_interaction.contains("metrics")) {
      const auto& pm = prev_interaction["metrics"];
      prev_input_tokens = pm.value("input_tokens", (int64_t)0);
      prev_output_tokens = pm.value("output_tokens", (int64_t)0);
      prev_cache_read_tokens = pm.value("cache_read_tokens", (int64_t)0);
      prev_cache_creation_tokens = pm.value("cache_creation_tokens", (int64_t)0);
      prev_cost_usd = pm.value("cost_usd", 0.0);
    }
    if (prev_interaction.contains("request") &&
        prev_interaction["request"].contains("body") &&
        prev_interaction["request"]["body"].contains("messages")) {
      prev_message_count = static_cast<int64_t>(
          prev_interaction["request"]["body"]["messages"].size());
    }
  }
  int64_t prev_effective_input_tokens =
      prev_input_tokens + prev_cache_read_tokens + prev_cache_creation_tokens;

  // 5. Determine event_type
  std::string event_type;
  if (parent_seq_id == 0) {
    event_type = "conversation_start";
  } else if (message_count < prev_message_count && prev_message_count > 0) {
    event_type = "compression";
  } else if (tool_call_count > 0) {
    event_type = "tool_execution";
  } else {
    event_type = "continuation";
  }

  // 6. Build diff node
  json diff_node;
  diff_node["sequence_id"] = seq_id;
  diff_node["session_id"] = session_id;
  diff_node["timestamp"] = interaction.value("timestamp", "");
  diff_node["model"] = interaction.value("model", "");

  // Billing tokens (uncached only)
  diff_node["total_input_tokens"] = input_tokens;
  diff_node["total_output_tokens"] = output_tokens;

  // Effective context-window tokens (billing + cached)
  diff_node["total_effective_input_tokens"] = effective_input_tokens;

  // Deltas
  diff_node["delta_input_tokens"] = input_tokens - prev_input_tokens;
  diff_node["delta_output_tokens"] = output_tokens - prev_output_tokens;
  diff_node["delta_effective_input_tokens"] = effective_input_tokens - prev_effective_input_tokens;

  // Message counts
  diff_node["message_count"] = message_count;
  diff_node["delta_messages"] = message_count - prev_message_count;

  // Cost and latency
  diff_node["cost_usd"] = cost_usd;
  diff_node["delta_cost_usd"] = cost_usd - prev_cost_usd;
  diff_node["latency_ms"] = latency_ms;
  diff_node["ttft_ms"] = ttft_ms;

  // CEE overhead (proxy_overhead_ms stored in interaction record; ctx_untangler_ms added below)
  double proxy_overhead_ms = 0.0;
  if (interaction.contains("metrics")) {
    proxy_overhead_ms = interaction["metrics"].value("proxy_overhead_ms", 0.0);
  }
  diff_node["proxy_overhead_ms"] = proxy_overhead_ms;

  // Event classification
  diff_node["event_type"] = event_type;

  // Conversation threading
  if (interaction.contains("conversation")) {
    diff_node["parent_sequence_id"] =
        interaction["conversation"].value("parent_sequence_id", (uint64_t)0);
    diff_node["conversation_id"] =
        interaction["conversation"].value("conversation_id", "");
    diff_node["turn_number"] =
        interaction["conversation"].value("turn_number", 0);
    diff_node["turn_type"] =
        interaction["conversation"].value("turn_type", "");
  }

  // Response metadata
  if (interaction.contains("response")) {
    diff_node["stop_reason"] =
        interaction["response"].value("stop_reason", "");
  }
  diff_node["tool_call_count"] = tool_call_count;

  // 7. Store in sister CTE bucket
  // Snapshot elapsed time so far (CTE write latency excluded from per-record value)
  if (logging) {
    auto diff_mid = std::chrono::steady_clock::now();
    diff_node["ctx_untangler_ms"] = std::chrono::duration<double, std::milli>(
        diff_mid - diff_start).count();
  }

  std::string graph_tag_name = BuildGraphTagName(session_id);
  std::string diff_data = diff_node.dump();
  try {
    auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(graph_tag_name);
    co_await tag_future;
    auto tag_id = tag_future->tag_id_;

    auto *ipc = CLIO_CPU_IPC;
    auto shm = ipc->AllocateBuffer(diff_data.size());
    if (shm.IsNull()) {
      HLOG(kError, "ComputeDiff: AllocateBuffer({}) failed for diff node", diff_data.size());
      task->success_ = 0;
      co_return;
    }
    memcpy(shm.ptr_, diff_data.c_str(), diff_data.size());
    auto put_future = CLIO_CTE_CLIENT->AsyncPutBlob(
        tag_id, blob_name, 0, diff_data.size(), ctp::ipc::ShmPtr<>(shm.shm_));
    co_await put_future;
    ipc->FreeBuffer(shm);
  } catch (const std::exception& e) {
    HLOG(kError, "ComputeDiff: failed to store diff node: {}", e.what());
    task->success_ = 0;
    co_return;
  }

  HLOG(kDebug, "Computed diff seq={} session={} event_type={}", seq_id,
       session_id, event_type);

  if (logging) {
    auto diff_end = std::chrono::steady_clock::now();
    uint64_t diff_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            diff_end - diff_start).count());
    total_diff_us_.fetch_add(diff_us, std::memory_order_relaxed);
  }
  diffs_computed_.fetch_add(1, std::memory_order_relaxed);

  task->success_ = 1;
  co_return;
}

clio::run::TaskResume Runtime::Monitor(clio::run::shared_ptr<MonitorTask> &task) {
  const std::string& query = task->query_;

  if (query == "list_graphs") {
    // Return all Ctx_graph_* session IDs
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);

    try {
      auto future = CLIO_CTE_CLIENT->AsyncTagQuery("Ctx_graph_.*");
      co_await future;
      auto tag_names = future->results_;
      const std::string prefix = "Ctx_graph_";

      pk.pack_array(static_cast<uint32_t>(tag_names.size()));
      for (const auto& tag_name : tag_names) {
        std::string session_id = tag_name;
        if (session_id.starts_with(prefix)) {
          session_id = session_id.substr(prefix.size());
        }
        pk.pack(session_id);
      }
    } catch (...) {
      pk.pack_array(0);
    }

    task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());

  } else if (query.rfind("query_graph://", 0) == 0) {
    // query_graph://<session_id> or query_graph://<session_id>?since=<seq>
    std::string body = query.substr(14);
    uint64_t since_seq = 0;
    auto qmark = body.find('?');
    std::string session_id;
    if (qmark != std::string::npos) {
      session_id = body.substr(0, qmark);
      std::string params = body.substr(qmark + 1);
      if (params.rfind("since=", 0) == 0) {
        try {
          since_seq = std::stoull(params.substr(6));
        } catch (...) {}
      }
    } else {
      session_id = body;
    }

    std::string graph_tag_name = BuildGraphTagName(session_id);
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);

    try {
      auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(graph_tag_name);
      co_await tag_future;
      auto tag_id = tag_future->tag_id_;

      auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
      co_await blobs_future;
      auto& blobs = blobs_future->blob_names_;

      // Filter by since_seq if specified
      std::vector<std::string> filtered;
      for (const auto& bname : blobs) {
        if (since_seq > 0) {
          try {
            uint64_t bseq = std::stoull(bname);
            if (bseq <= since_seq) continue;
          } catch (...) {}
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

  } else if (query == "overhead_logging:on") {
    overhead_logging_.store(true, std::memory_order_relaxed);
    msgpack::sbuffer sbuf; msgpack::packer<msgpack::sbuffer> pk(sbuf);
    pk.pack_map(1); pk.pack("enabled"); pk.pack(true);
    task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  } else if (query == "overhead_logging:off") {
    overhead_logging_.store(false, std::memory_order_relaxed);
    msgpack::sbuffer sbuf; msgpack::packer<msgpack::sbuffer> pk(sbuf);
    pk.pack_map(1); pk.pack("enabled"); pk.pack(false);
    task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  } else if (query == "overhead_logging:status") {
    msgpack::sbuffer sbuf; msgpack::packer<msgpack::sbuffer> pk(sbuf);
    pk.pack_map(1); pk.pack("enabled");
    pk.pack(overhead_logging_.load(std::memory_order_relaxed));
    task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  } else if (query == "overhead_stats") {
    uint64_t count = diffs_computed_.load(std::memory_order_relaxed);
    uint64_t total_us = total_diff_us_.load(std::memory_order_relaxed);
    double avg_ms = count > 0
        ? static_cast<double>(total_us) / count / 1000.0 : 0.0;

    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);
    pk.pack_map(3);
    pk.pack("diffs_computed"); pk.pack(count);
    pk.pack("total_diff_us"); pk.pack(total_us);
    pk.pack("avg_diff_ms"); pk.pack(avg_ms);
    task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());

  } else if (query.rfind("get_node://", 0) == 0) {
    // get_node://<session_id>/<seq_id>
    std::string body = query.substr(11);
    auto slash = body.find('/');
    if (slash != std::string::npos) {
      std::string session_id = body.substr(0, slash);
      uint64_t seq_id = std::stoull(body.substr(slash + 1));
      std::string graph_tag_name = BuildGraphTagName(session_id);
      std::string blob_name = FormatBlobName(seq_id);

      msgpack::sbuffer sbuf;
      msgpack::packer<msgpack::sbuffer> pk(sbuf);

      try {
        auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(graph_tag_name);
        co_await tag_future;
        auto tag_id = tag_future->tag_id_;

        auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, blob_name);
        co_await size_future;
        auto size = size_future->size_;

        if (size > 0) {
          auto *ipc = CLIO_CPU_IPC;
          auto shm = ipc->AllocateBuffer(size);
          if (shm.IsNull()) { pk.pack("{}"); } else {
          auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
              tag_id, blob_name, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
          co_await get_future;
          pk.pack(std::string(reinterpret_cast<char*>(shm.ptr_), size));
          ipc->FreeBuffer(shm); }
        } else {
          pk.pack("{}");
        }
      } catch (...) {
        pk.pack("{}");
      }

      task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
    }
  }
  co_return;
}

clio::run::TaskResume Runtime::Destroy(clio::run::shared_ptr<DestroyTask> &task) {
  HLOG(kInfo, "Context Untangler ChiMod destroyed");
  (void)task;
  co_return;
}

clio::run::u64 Runtime::GetWorkRemaining() const { return 0; }

}  // namespace dt_provenance::ctx_untangler

CLIO_TASK_CC(dt_provenance::ctx_untangler::Runtime)
