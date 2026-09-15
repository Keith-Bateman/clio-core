#include "dt_provenance/dt_tracker/tracker_runtime.h"

#include <clio_cte/core/core_client.h>
#include <clio_ctp/serialize/msgpack_wrapper.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>

#include "dt_provenance/dt_ctx_untangler/ctx_untangler_client.h"
#include "dt_provenance/dt_tracker/failure_detector.h"

namespace dt_provenance::tracker {

using json = nlohmann::ordered_json;

Runtime::~Runtime() = default;

clio::run::TaskStat Runtime::GetTaskStats(const clio::run::Task *task) const {
  // StoreInteraction performs CTE PutBlob I/O — report io_size >= 4096
  // so the DefaultScheduler routes it to an I/O worker instead of worker 0.
  if (task->method_ == Method::kStoreInteraction) {
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

std::string Runtime::BuildTagName(const std::string& session_id) {
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
        HLOG(kInfo, "Tracker: initialized CTE client with pool_id={}", cte_pool);
      }
    }
  }

  // Recover sequence_counter_ from existing CTE tags
  try {
    auto tags = CLIO_CTE_CLIENT->AsyncTagQuery("Agentic_session_.*");
    co_await tags;
    auto tag_names = tags->results_;
    uint64_t max_seq = 0;
    for (const auto& tname : tag_names) {
      auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tname);
      co_await tag_future;
      auto tag_id = tag_future->tag_id_;
      auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
      co_await blobs_future;
      auto& blobs = blobs_future->blob_names_;
      for (const auto& bname : blobs) {
        try {
          uint64_t seq = std::stoull(bname);
          if (seq > max_seq) max_seq = seq;
        } catch (...) {}
      }
    }
    if (max_seq > 0) {
      sequence_counter_.store(max_seq);
      HLOG(kInfo, "Recovered sequence_counter_ to {}", max_seq);
    }
  } catch (...) {
    HLOG(kWarning, "CTE tag scan failed during Create — starting fresh");
  }

  HLOG(kInfo, "Conversation Tracker ChiMod created (CTE-backed)");
  co_return;
}

clio::run::TaskResume Runtime::StoreInteraction(
    clio::run::shared_ptr<StoreInteractionTask> &task) {
  const bool logging = overhead_logging_.load(std::memory_order_relaxed);
  auto store_start = logging ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};

  // 1. Atomic increment for monotonic sequence ID
  uint64_t seq_id = sequence_counter_.fetch_add(1) + 1;

  // 2. Parse interaction JSON
  std::string interaction_str(task->interaction_json_.str());
  json interaction;
  try {
    interaction = json::parse(interaction_str);
  } catch (const json::parse_error& e) {
    HLOG(kError, "Failed to parse interaction JSON: {}", e.what());
    task->sequence_id_ = 0;
    co_return;
  }

  // 3. Set sequence_id in the interaction
  interaction["sequence_id"] = seq_id;

  // 4. Extract session_id
  std::string session_id = interaction.value("session_id", "default");
  std::string tag_name = BuildTagName(session_id);
  std::string blob_name = FormatBlobName(seq_id);

  // 5. Resolve conversation threading
  auto record = dt_provenance::protocol::InteractionRecord::FromJson(interaction);
  record.sequence_id = seq_id;
  threader_.ResolveThreading(record);
  interaction["conversation"] = record.conversation.ToJson();

  // 6. Store in CTE (co_await instead of blocking Tag)
  std::string data = interaction.dump();
  try {
    auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
    co_await tag_future;
    auto tag_id = tag_future->tag_id_;

    auto *ipc = CLIO_CPU_IPC;
    auto shm = ipc->AllocateBuffer(data.size());
    if (shm.IsNull()) {
      HLOG(kError, "StoreInteraction: AllocateBuffer({}) failed", data.size());
      task->sequence_id_ = 0;
      co_return;
    }
    memcpy(shm.ptr_, data.c_str(), data.size());
    auto put_future = CLIO_CTE_CLIENT->AsyncPutBlob(
        tag_id, blob_name, 0, data.size(), ctp::ipc::ShmPtr<>(shm.shm_));
    co_await put_future;
    ipc->FreeBuffer(shm);
  } catch (const std::exception& e) {
    HLOG(kError, "CTE PutBlob failed: {}", e.what());
    task->sequence_id_ = 0;
    co_return;
  }

  HLOG(kDebug, "Stored interaction seq={} tag={} blob={}", seq_id, tag_name,
       blob_name);

  // 7. Auto-generate recovery events for detected failures (best-effort)
  {
    auto failure_events = FailureDetector::Detect(interaction);
    for (auto& event : failure_events) {
      std::string recovery_tag = "Recovery_" + session_id;
      std::string event_id = event.value("event_id", "");
      std::string event_data = event.dump();
      try {
        auto rtag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(recovery_tag);
        co_await rtag_future;
        auto rtag_id = rtag_future->tag_id_;

        auto *ipc = CLIO_CPU_IPC;
        auto shm = ipc->AllocateBuffer(event_data.size());
        if (!shm.IsNull()) {
          memcpy(shm.ptr_, event_data.c_str(), event_data.size());
          auto put_future = CLIO_CTE_CLIENT->AsyncPutBlob(
              rtag_id, event_id, 0, event_data.size(), ctp::ipc::ShmPtr<>(shm.shm_));
          co_await put_future;
          ipc->FreeBuffer(shm);
          HLOG(kInfo, "Auto recovery event: session={} type={} event_id={}",
               session_id, event["payload"].value("error_type", ""), event_id);
        }
      } catch (const std::exception& e) {
        HLOG(kWarning, "Failed to store auto recovery event: {}", e.what());
      }
    }
  }

  // 8. Write to DTP_session_index on first interaction per session (best-effort).
  if (indexed_sessions_.find(session_id) == indexed_sessions_.end()) {
    co_await _WriteSessionIndex(session_id, interaction);
    indexed_sessions_.insert(session_id);
  }

  // 9. Dispatch to Ctx Untangler for diff computation
  if (!untangler_initialized_) {
    clio::run::PoolId pool = CLIO_POOL_MANAGER->FindPoolByName("dt_ctx_untangler_pool");
    if (!pool.IsNull()) {
      untangler_client_ = std::make_unique<dt_provenance::ctx_untangler::Client>(pool);
      untangler_initialized_ = true;
    }
  }
  if (untangler_initialized_) {
    auto f = untangler_client_->AsyncComputeDiff(
        clio::run::PoolQuery::Local(), session_id, seq_id);
    co_await f;
  }

  if (logging) {
    auto store_end = std::chrono::steady_clock::now();
    uint64_t store_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            store_end - store_start).count());
    total_store_us_.fetch_add(store_us, std::memory_order_relaxed);
  }
  interactions_stored_.fetch_add(1, std::memory_order_relaxed);

  task->sequence_id_ = seq_id;
  co_return;
}

clio::run::TaskResume Runtime::_WriteSessionIndex(const std::string& session_id,
                                             const json& interaction) {
  // Write a lightweight metadata blob to DTP_session_index so that
  // HandleListSessions can enumerate sessions in O(n) without scanning
  // every Agentic_session_* tag for its blob count.
  const std::string index_tag = "DTP_session_index";
  json meta{
      {"session_id", session_id},
      {"first_sequence_id", interaction.value("sequence_id", 0)},
      {"timestamp", interaction.value("timestamp", "")},
  };
  std::string meta_str = meta.dump();
  try {
    auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(index_tag);
    co_await tag_future;
    auto tag_id = tag_future->tag_id_;

    auto *ipc = CLIO_CPU_IPC;
    auto shm = ipc->AllocateBuffer(meta_str.size());
    if (shm.IsNull()) {
      HLOG(kWarning, "_WriteSessionIndex: AllocateBuffer failed for session={}",
           session_id);
      co_return;
    }
    memcpy(shm.ptr_, meta_str.c_str(), meta_str.size());
    auto put_future = CLIO_CTE_CLIENT->AsyncPutBlob(
        tag_id, session_id, 0, meta_str.size(), ctp::ipc::ShmPtr<>(shm.shm_));
    co_await put_future;
    ipc->FreeBuffer(shm);
    HLOG(kDebug, "Session index updated: session={}", session_id);
  } catch (const std::exception& e) {
    HLOG(kWarning, "_WriteSessionIndex failed for session={}: {}", session_id, e.what());
  }
  co_return;
}

clio::run::TaskResume Runtime::QuerySession(clio::run::shared_ptr<QuerySessionTask> &task) {
  std::string session_id(task->session_id_.str());
  std::string tag_name = BuildTagName(session_id);

  json result = json::array();
  try {
    auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
    co_await tag_future;
    auto tag_id = tag_future->tag_id_;

    auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
    co_await blobs_future;
    auto& blobs = blobs_future->blob_names_;

    auto *ipc = CLIO_CPU_IPC;
    for (const auto& bname : blobs) {
      auto size_future = CLIO_CTE_CLIENT->AsyncGetBlobSize(tag_id, bname);
      co_await size_future;
      auto size = size_future->size_;
      if (size == 0) continue;

      auto shm = ipc->AllocateBuffer(size);
      if (shm.IsNull()) continue;
      auto get_future = CLIO_CTE_CLIENT->AsyncGetBlob(
          tag_id, bname, 0, size, 0, ctp::ipc::ShmPtr<>(shm.shm_));
      co_await get_future;
      try {
        result.push_back(json::parse(
            std::string(reinterpret_cast<char*>(shm.ptr_), size)));
      } catch (const json::parse_error&) {}
      ipc->FreeBuffer(shm);
    }
  } catch (...) {
    // Tag doesn't exist yet — return empty array
  }

  task->interactions_json_ = result.dump();
  co_return;
}

clio::run::TaskResume Runtime::ListSessions(clio::run::shared_ptr<ListSessionsTask> &task) {
  json result = json::array();
  try {
    auto future = CLIO_CTE_CLIENT->AsyncTagQuery("Agentic_session_.*");
    co_await future;
    auto tag_names = future->results_;
    const std::string prefix = "Agentic_session_";
    for (const auto& tag_name : tag_names) {
      std::string session_id = tag_name;
      if (session_id.starts_with(prefix)) {
        session_id = session_id.substr(prefix.size());
      }
      auto tag_future = CLIO_CTE_CLIENT->AsyncGetOrCreateTag(tag_name);
      co_await tag_future;
      auto tag_id = tag_future->tag_id_;

      auto blobs_future = CLIO_CTE_CLIENT->AsyncGetContainedBlobs(tag_id);
      co_await blobs_future;
      auto& blobs = blobs_future->blob_names_;

      result.push_back(json{
          {"session_id", session_id},
          {"count", blobs.size()},
          {"tag_name", tag_name}});
    }
  } catch (...) {
    // CTE not available — return empty
  }

  task->sessions_json_ = result.dump();
  co_return;
}

clio::run::TaskResume Runtime::GetInteraction(
    clio::run::shared_ptr<GetInteractionTask> &task) {
  std::string session_id(task->session_id_.str());
  uint64_t seq_id = task->sequence_id_;
  std::string tag_name = BuildTagName(session_id);
  std::string blob_name = FormatBlobName(seq_id);

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
      task->interaction_json_ = std::string(
          reinterpret_cast<char*>(shm.ptr_), size);
      ipc->FreeBuffer(shm);
      co_return;
      }
    }
  } catch (...) {}

  task->interaction_json_ = "{}";
  co_return;
}

clio::run::TaskResume Runtime::Monitor(clio::run::shared_ptr<MonitorTask> &task) {
  const std::string& query = task->query_;
  HLOG(kInfo, "Tracker Monitor: query='{}'", query);

  if (query == "overhead_logging:on") {
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
    uint64_t count = interactions_stored_.load(std::memory_order_relaxed);
    uint64_t total_us = total_store_us_.load(std::memory_order_relaxed);
    double avg_ms = count > 0
        ? static_cast<double>(total_us) / count / 1000.0 : 0.0;

    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);
    pk.pack_map(3);
    pk.pack("interactions_stored"); pk.pack(count);
    pk.pack("total_store_us"); pk.pack(total_us);
    pk.pack("avg_store_ms"); pk.pack(avg_ms);
    task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  }
  co_return;
}

clio::run::TaskResume Runtime::Destroy(clio::run::shared_ptr<DestroyTask> &task) {
  HLOG(kInfo, "Conversation Tracker ChiMod destroyed");
  untangler_client_.reset();
  (void)task;
  co_return;
}

clio::run::u64 Runtime::GetWorkRemaining() const { return 0; }

}  // namespace dt_provenance::tracker

CLIO_TASK_CC(dt_provenance::tracker::Runtime)
