#ifndef DT_PROVENANCE_TRACKER_RUNTIME_H_
#define DT_PROVENANCE_TRACKER_RUNTIME_H_

#include <atomic>
#include <clio_runtime/clio_runtime.h>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

#include "autogen/tracker_methods.h"
#include "conversation_threading.h"
#include "tracker_client.h"
#include "tracker_tasks.h"

// Forward-declare untangler client to avoid circular header deps
namespace dt_provenance::ctx_untangler {
class Client;
}

namespace dt_provenance::tracker {

/**
 * Runtime container for the Conversation Tracker ChiMod
 *
 * Stores interaction records using CTE's Tag/Blob model:
 * - Tag = "Agentic_session_{session_id}"
 * - Blob = zero-padded monotonic counter (e.g., "0000000001")
 *
 * Uses CTE for persistent storage across restarts.
 */
class Runtime : public clio::run::Container {
 public:
  using CreateParams = dt_provenance::tracker::CreateParams;

  Runtime() = default;
  ~Runtime() override;

  // Container interface
  void Init(const clio::run::PoolId& pool_id, const std::string& pool_name,
            clio::run::u32 container_id = 0) override;
  clio::run::TaskResume Run(clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> task_ptr) override;

  // Method handlers
  clio::run::TaskResume Create(clio::run::shared_ptr<CreateTask> &task);
  clio::run::TaskResume StoreInteraction(clio::run::shared_ptr<StoreInteractionTask> &task);
  clio::run::TaskResume QuerySession(clio::run::shared_ptr<QuerySessionTask> &task);
  clio::run::TaskResume ListSessions(clio::run::shared_ptr<ListSessionsTask> &task);
  clio::run::TaskResume GetInteraction(clio::run::shared_ptr<GetInteractionTask> &task);
  clio::run::TaskResume Monitor(clio::run::shared_ptr<MonitorTask> &task);
  clio::run::TaskResume Destroy(clio::run::shared_ptr<DestroyTask> &task);

  // Container virtual methods
  clio::run::TaskStat GetTaskStats(const clio::run::Task *task) const override;
  clio::run::u64 GetWorkRemaining() const override;
  void SaveTask(clio::run::u32 method, clio::run::SaveTaskArchive& archive,
                clio::run::shared_ptr<clio::run::Task> &task_ptr) override;
  void LoadTask(clio::run::u32 method, clio::run::LoadTaskArchive& archive,
                clio::run::shared_ptr<clio::run::Task> &task_ptr) override;
  clio::run::shared_ptr<clio::run::Task> AllocLoadTask(
      clio::run::u32 method, clio::run::LoadTaskArchive& archive) override;
  void LocalLoadTask(clio::run::u32 method, clio::run::DefaultLoadArchive& archive,
                     clio::run::shared_ptr<clio::run::Task> &task_ptr) override;
  clio::run::shared_ptr<clio::run::Task> LocalAllocLoadTask(
      clio::run::u32 method, clio::run::DefaultLoadArchive& archive) override;
  void LocalSaveTask(clio::run::u32 method, clio::run::DefaultSaveArchive& archive,
                     clio::run::shared_ptr<clio::run::Task> &task_ptr) override;
  clio::run::shared_ptr<clio::run::Task> NewCopyTask(clio::run::u32 method,
                                       clio::run::shared_ptr<clio::run::Task> &orig_task_ptr,
                                       bool deep) override;
  clio::run::shared_ptr<clio::run::Task> NewTask(clio::run::u32 method) override;
  void AggregateOut(clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> &orig_task,
                 const clio::run::shared_ptr<clio::run::Task>& replica_task) override;
  void AggregateIn(clio::run::u32 method, clio::run::shared_ptr<clio::run::Task>& agg_task,
                   const clio::run::shared_ptr<clio::run::Task>& member_task) override;

 private:
  /** Format sequence_id as a zero-padded blob name */
  static std::string FormatBlobName(uint64_t seq_id);

  /** Build CTE tag name from session_id */
  static std::string BuildTagName(const std::string& session_id);

  /** Write a lightweight metadata record to DTP_session_index for fast listing. */
  clio::run::TaskResume _WriteSessionIndex(const std::string& session_id,
                                     const nlohmann::ordered_json& interaction);

  Client client_;
  std::atomic<uint64_t> sequence_counter_{0};
  ConversationThreader threader_;

  // Sessions already written to DTP_session_index (avoids redundant writes).
  std::unordered_set<std::string> indexed_sessions_;

  // Overhead tracking
  std::atomic<uint64_t> total_store_us_{0};     // cumulative StoreInteraction time
  std::atomic<uint64_t> interactions_stored_{0};
  std::atomic<bool> overhead_logging_{true};

  // Ctx Untangler lazy-init (dispatches ComputeDiff after storing)
  std::unique_ptr<dt_provenance::ctx_untangler::Client> untangler_client_;
  bool untangler_initialized_ = false;
};

}  // namespace dt_provenance::tracker

#endif  // DT_PROVENANCE_TRACKER_RUNTIME_H_
