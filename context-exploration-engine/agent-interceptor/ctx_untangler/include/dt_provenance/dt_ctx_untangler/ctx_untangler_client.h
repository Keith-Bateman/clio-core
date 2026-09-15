#ifndef DT_PROVENANCE_CTX_UNTANGLER_CLIENT_H_
#define DT_PROVENANCE_CTX_UNTANGLER_CLIENT_H_

#include <clio_runtime/clio_runtime.h>

#include "ctx_untangler_tasks.h"

namespace dt_provenance::ctx_untangler {

/**
 * Client API for the Ctx Untangler ChiMod
 */
class Client : public clio::run::ContainerClient {
 public:
  Client() = default;
  explicit Client(const clio::run::PoolId& pool_id) { Init(pool_id); }

  /** Create the untangler container */
  clio::run::Future<CreateTask> AsyncCreate(const clio::run::PoolQuery& pool_query,
                                      const std::string& pool_name,
                                      const clio::run::PoolId& custom_pool_id) {
    auto* ipc = CLIO_CPU_IPC;
    auto task = ipc->NewTask<CreateTask>(
        clio::run::CreateTaskId(), clio::run::kAdminPoolId, pool_query,
        CreateParams::chimod_lib_name, pool_name, custom_pool_id, this);
    return ipc->Send(task);
  }

  /** Compute diff for a newly-stored interaction */
  clio::run::Future<ComputeDiffTask> AsyncComputeDiff(
      const clio::run::PoolQuery& pool_query,
      const std::string& session_id, clio::run::u64 sequence_id) {
    auto* ipc = CLIO_CPU_IPC;
    auto task = ipc->NewTask<ComputeDiffTask>(
        clio::run::CreateTaskId(), pool_id_, pool_query, session_id, sequence_id);
    return ipc->Send(task);
  }

  /** Monitor container state */
  clio::run::Future<MonitorTask> AsyncMonitor(const clio::run::PoolQuery& pool_query,
                                        const std::string& query) {
    auto* ipc = CLIO_CPU_IPC;
    auto task = ipc->NewTask<MonitorTask>(
        clio::run::CreateTaskId(), pool_id_, pool_query, query);
    return ipc->Send(task);
  }
};

}  // namespace dt_provenance::ctx_untangler

#endif  // DT_PROVENANCE_CTX_UNTANGLER_CLIENT_H_
