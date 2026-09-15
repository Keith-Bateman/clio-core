#ifndef DT_PROVENANCE_CTX_UNTANGLER_RUNTIME_H_
#define DT_PROVENANCE_CTX_UNTANGLER_RUNTIME_H_

#include <atomic>
#include <clio_runtime/clio_runtime.h>
#include <string>

#include "autogen/ctx_untangler_methods.h"
#include "ctx_untangler_client.h"
#include "ctx_untangler_tasks.h"

namespace dt_provenance::ctx_untangler {

/**
 * Runtime container for the Context Untangler ChiMod
 *
 * Eagerly computes diffs when interactions arrive and stores
 * them in sister CTE buckets (Ctx_graph_<session_id>).
 */
class Runtime : public clio::run::Container {
 public:
  using CreateParams = dt_provenance::ctx_untangler::CreateParams;

  Runtime() = default;
  ~Runtime() override = default;

  // Container interface
  void Init(const clio::run::PoolId& pool_id, const std::string& pool_name,
            clio::run::u32 container_id = 0) override;
  clio::run::TaskResume Run(clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> task_ptr) override;

  // Method handlers
  clio::run::TaskResume Create(clio::run::shared_ptr<CreateTask> &task);
  clio::run::TaskResume ComputeDiff(clio::run::shared_ptr<ComputeDiffTask> &task);
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

  /** Build CTE graph tag name from session_id */
  static std::string BuildGraphTagName(const std::string& session_id);

  /** Build CTE interaction tag name from session_id */
  static std::string BuildInteractionTagName(const std::string& session_id);

  Client client_;

  // Overhead tracking
  std::atomic<uint64_t> total_diff_us_{0};    // cumulative ComputeDiff time
  std::atomic<uint64_t> diffs_computed_{0};
  std::atomic<bool> overhead_logging_{true};
};

}  // namespace dt_provenance::ctx_untangler

#endif  // DT_PROVENANCE_CTX_UNTANGLER_RUNTIME_H_
