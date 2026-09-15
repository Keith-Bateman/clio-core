#ifndef DT_PROVENANCE_CTX_UNTANGLER_TASKS_H_
#define DT_PROVENANCE_CTX_UNTANGLER_TASKS_H_

#include <clio_runtime/admin/admin_tasks.h>
#include <clio_runtime/clio_runtime.h>
#include <clio_runtime/config_manager.h>
#include <yaml-cpp/yaml.h>

#include "autogen/ctx_untangler_methods.h"

namespace dt_provenance::ctx_untangler {

using MonitorTask = clio::run::admin::MonitorTask;

/**
 * CreateParams for the Ctx Untangler ChiMod
 */
struct CreateParams {
  static constexpr const char* chimod_lib_name = "dt_provenance_dt_ctx_untangler";

  CreateParams() = default;

  template <class Archive>
  void serialize(Archive& ar) {
    (void)ar;
  }

  void LoadConfig(const clio::run::PoolConfig& pool_config) {
    (void)pool_config;
  }
};

using CreateTask = clio::run::admin::GetOrCreatePoolTask<CreateParams>;
using DestroyTask = clio::run::admin::DestroyTask;

/**
 * ComputeDiffTask — compute diff for a newly-stored interaction
 */
struct ComputeDiffTask : public clio::run::Task {
  IN clio::run::priv::string session_id_;
  IN clio::run::u64 sequence_id_;
  OUT clio::run::u64 success_;

  ComputeDiffTask()
      : clio::run::Task(), session_id_(CLIO_PRIV_ALLOC), sequence_id_(0), success_(0) {}

  explicit ComputeDiffTask(const clio::run::TaskId& task_node,
                           const clio::run::PoolId& pool_id,
                           const clio::run::PoolQuery& pool_query,
                           const std::string& session_id,
                           clio::run::u64 sequence_id)
      : clio::run::Task(task_node, pool_id, pool_query, 10),
        session_id_(CLIO_PRIV_ALLOC),
        sequence_id_(sequence_id),
        success_(0) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kComputeDiff;
    task_flags_.Clear();
    pool_query_ = pool_query;
    session_id_ = session_id;
  }

  template <typename Archive>
  void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(session_id_, sequence_id_);
  }

  template <typename Archive>
  void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(success_);
  }

  void Copy(const ctp::ipc::FullPtr<ComputeDiffTask>& other) {
    Task::Copy(other.template Cast<Task>());
    session_id_ = other->session_id_;
    sequence_id_ = other->sequence_id_;
    success_ = other->success_;
  }

  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task>& other_base) {
    Task::AggregateOut(other_base);
    Copy(other_base.template Cast<ComputeDiffTask>());
  }
};

}  // namespace dt_provenance::ctx_untangler

#endif  // DT_PROVENANCE_CTX_UNTANGLER_TASKS_H_
