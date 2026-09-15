#ifndef DT_PROVENANCE_INTERCEPTION_OLLAMA_CLIENT_H_
#define DT_PROVENANCE_INTERCEPTION_OLLAMA_CLIENT_H_

#include <clio_runtime/clio_runtime.h>

#include "ollama_tasks.h"

namespace dt_provenance::intercept_ollama {

/**
 * Client API for the Ollama Interception ChiMod
 */
class Client : public clio::run::ContainerClient {
 public:
  Client() = default;
  explicit Client(const clio::run::PoolId& pool_id) { Init(pool_id); }

  /** Create the Ollama interception container */
  clio::run::Future<CreateTask> AsyncCreate(const clio::run::PoolQuery& pool_query,
                                      const std::string& pool_name,
                                      const clio::run::PoolId& custom_pool_id,
                                      const std::string& upstream_base_url) {
    auto* ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<CreateTask>(
        clio::run::CreateTaskId(), clio::run::kAdminPoolId, pool_query,
        CreateParams::chimod_lib_name, pool_name, custom_pool_id, this,
        upstream_base_url);
    return ipc_manager->Send(task);
  }

  /**
   * Intercept and forward a request to Ollama
   * @param session_id Session identifier
   * @param path API path (e.g., "/api/chat" or "/api/generate")
   * @param headers_json JSON-serialized request headers
   * @param request_body Request body
   * @param request_time_ns Timestamp when request was received (nanoseconds)
   */
  clio::run::Future<InterceptAndForwardTask> AsyncInterceptAndForward(
      const clio::run::PoolQuery& pool_query, const std::string& session_id,
      const std::string& path, const std::string& headers_json,
      const std::string& request_body, clio::run::u64 request_time_ns) {
    auto* ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<InterceptAndForwardTask>(
        clio::run::CreateTaskId(), pool_id_, pool_query, session_id, path,
        headers_json, request_body, request_time_ns);
    return ipc_manager->Send(task);
  }

  /** Monitor container state */
  clio::run::Future<MonitorTask> AsyncMonitor(const clio::run::PoolQuery& pool_query,
                                        const std::string& query) {
    auto* ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<MonitorTask>(clio::run::CreateTaskId(),
                                                   pool_id_, pool_query, query);
    return ipc_manager->Send(task);
  }
};

}  // namespace dt_provenance::intercept_ollama

#endif  // DT_PROVENANCE_INTERCEPTION_OLLAMA_CLIENT_H_
