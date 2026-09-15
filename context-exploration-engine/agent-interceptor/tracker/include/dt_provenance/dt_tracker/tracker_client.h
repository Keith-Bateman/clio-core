#ifndef DT_PROVENANCE_TRACKER_CLIENT_H_
#define DT_PROVENANCE_TRACKER_CLIENT_H_

#include <clio_runtime/clio_runtime.h>

#include "tracker_tasks.h"

namespace dt_provenance::tracker {

/**
 * Client API for the Conversation Tracker ChiMod
 */
class Client : public clio::run::ContainerClient {
 public:
  Client() = default;
  explicit Client(const clio::run::PoolId& pool_id) { Init(pool_id); }

  /** Create the tracker container */
  clio::run::Future<CreateTask> AsyncCreate(const clio::run::PoolQuery& pool_query,
                                      const std::string& pool_name,
                                      const clio::run::PoolId& custom_pool_id) {
    auto* ipc = CLIO_CPU_IPC;
    auto task = ipc->NewTask<CreateTask>(
        clio::run::CreateTaskId(), clio::run::kAdminPoolId, pool_query,
        CreateParams::chimod_lib_name, pool_name, custom_pool_id, this);
    return ipc->Send(task);
  }

  /** Store an interaction record */
  clio::run::Future<StoreInteractionTask> AsyncStoreInteraction(
      const clio::run::PoolQuery& pool_query,
      const std::string& interaction_json) {
    auto* ipc = CLIO_CPU_IPC;
    auto task = ipc->NewTask<StoreInteractionTask>(
        clio::run::CreateTaskId(), pool_id_, pool_query, interaction_json);
    return ipc->Send(task);
  }

  /** Query all interactions in a session */
  clio::run::Future<QuerySessionTask> AsyncQuerySession(
      const clio::run::PoolQuery& pool_query,
      const std::string& session_id) {
    auto* ipc = CLIO_CPU_IPC;
    auto task = ipc->NewTask<QuerySessionTask>(
        clio::run::CreateTaskId(), pool_id_, pool_query, session_id);
    return ipc->Send(task);
  }

  /** List all sessions */
  clio::run::Future<ListSessionsTask> AsyncListSessions(
      const clio::run::PoolQuery& pool_query) {
    auto* ipc = CLIO_CPU_IPC;
    auto task = ipc->NewTask<ListSessionsTask>(
        clio::run::CreateTaskId(), pool_id_, pool_query);
    return ipc->Send(task);
  }

  /** Get a single interaction by session + sequence_id */
  clio::run::Future<GetInteractionTask> AsyncGetInteraction(
      const clio::run::PoolQuery& pool_query,
      const std::string& session_id, clio::run::u64 sequence_id) {
    auto* ipc = CLIO_CPU_IPC;
    auto task = ipc->NewTask<GetInteractionTask>(
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

}  // namespace dt_provenance::tracker

#endif  // DT_PROVENANCE_TRACKER_CLIENT_H_
