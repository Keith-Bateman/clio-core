#ifndef DT_PROVENANCE_PROXY_CLIENT_H_
#define DT_PROVENANCE_PROXY_CLIENT_H_

#include <clio_runtime/clio_runtime.h>

#include "proxy_tasks.h"

namespace dt_provenance::proxy {

/**
 * Client API for the HTTP Proxy ChiMod
 *
 * The proxy is a lifecycle-only ChiMod: Create starts the HTTP server,
 * Destroy stops it. No custom task methods needed — HTTP threads dispatch
 * directly to interception ChiMod clients.
 */
class Client : public clio::run::ContainerClient {
 public:
  Client() = default;
  explicit Client(const clio::run::PoolId& pool_id) { Init(pool_id); }

  /** Create the proxy container (starts HTTP server) */
  clio::run::Future<CreateTask> AsyncCreate(const clio::run::PoolQuery& pool_query,
                                      const std::string& pool_name,
                                      const clio::run::PoolId& custom_pool_id,
                                      uint16_t port = 9090,
                                      uint16_t num_threads = 8) {
    auto* ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<CreateTask>(
        clio::run::CreateTaskId(), clio::run::kAdminPoolId, pool_query,
        CreateParams::chimod_lib_name, pool_name, custom_pool_id, this,
        port, num_threads);
    return ipc_manager->Send(task);
  }

  /** Monitor proxy state */
  clio::run::Future<MonitorTask> AsyncMonitor(const clio::run::PoolQuery& pool_query,
                                        const std::string& query) {
    auto* ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<MonitorTask>(clio::run::CreateTaskId(),
                                                   pool_id_, pool_query, query);
    return ipc_manager->Send(task);
  }

  /** Forward HTTP request to upstream API (runs on I/O worker) */
  clio::run::Future<ForwardHttpTask> AsyncForwardHttp(
      const clio::run::PoolQuery& pool_query, const std::string& query_json) {
    auto* ipc = CLIO_CPU_IPC;
    auto task = ipc->NewTask<ForwardHttpTask>(
        clio::run::CreateTaskId(), pool_id_, pool_query, query_json);
    return ipc->Send(task);
  }
};

}  // namespace dt_provenance::proxy

#endif  // DT_PROVENANCE_PROXY_CLIENT_H_
