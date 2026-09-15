#ifndef DT_PROVENANCE_PROXY_TASKS_H_
#define DT_PROVENANCE_PROXY_TASKS_H_

#include <clio_runtime/admin/admin_tasks.h>
#include <clio_runtime/clio_runtime.h>
#include <clio_runtime/config_manager.h>
#include <yaml-cpp/yaml.h>

#include "autogen/proxy_methods.h"

namespace dt_provenance::proxy {

using MonitorTask = clio::run::admin::MonitorTask;

/**
 * CreateParams for the HTTP Proxy ChiMod
 */
struct CreateParams {
  uint16_t port_;
  uint16_t num_threads_;

  static constexpr const char* chimod_lib_name = "dt_provenance_dt_proxy";

  CreateParams() : port_(9090), num_threads_(8) {}

  CreateParams(uint16_t port, uint16_t num_threads)
      : port_(port), num_threads_(num_threads) {}

  template <class Archive>
  void serialize(Archive& ar) {
    ar(port_, num_threads_);
  }

  /** Load from compose YAML config */
  void LoadConfig(const clio::run::PoolConfig& pool_config) {
    YAML::Node config = YAML::Load(pool_config.config_);
    if (config["port"]) {
      port_ = config["port"].as<uint16_t>();
    }
    if (config["num_threads"]) {
      num_threads_ = config["num_threads"].as<uint16_t>();
    }
  }
};

using CreateTask = clio::run::admin::GetOrCreatePoolTask<CreateParams>;
using DestroyTask = clio::run::admin::DestroyTask;

/**
 * ForwardHttpTask — forward an HTTP request to upstream API on an I/O worker
 */
struct ForwardHttpTask : public clio::run::Task {
  IN clio::run::priv::string query_json_;        // Full JSON forward request
  OUT clio::run::priv::string response_msgpack_; // Msgpack {status, headers, body}
  OUT clio::run::priv::string record_json_;      // Interaction record for tracker

  ForwardHttpTask()
      : clio::run::Task(),
        query_json_(CLIO_PRIV_ALLOC),
        response_msgpack_(CLIO_PRIV_ALLOC),
        record_json_(CLIO_PRIV_ALLOC) {}

  explicit ForwardHttpTask(const clio::run::TaskId& task_node,
                           const clio::run::PoolId& pool_id,
                           const clio::run::PoolQuery& pool_query,
                           const std::string& query_json)
      : clio::run::Task(task_node, pool_id, pool_query, 10),
        query_json_(CLIO_PRIV_ALLOC),
        response_msgpack_(CLIO_PRIV_ALLOC),
        record_json_(CLIO_PRIV_ALLOC) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kForwardHttp;
    task_flags_.Clear();
    pool_query_ = pool_query;
    query_json_ = query_json;
  }

  template <typename Archive>
  void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(query_json_);
  }

  template <typename Archive>
  void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(response_msgpack_, record_json_);
  }

  void Copy(const ctp::ipc::FullPtr<ForwardHttpTask>& other) {
    Task::Copy(other.template Cast<Task>());
    query_json_ = other->query_json_;
    response_msgpack_ = other->response_msgpack_;
    record_json_ = other->record_json_;
  }

  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task>& other_base) {
    Task::AggregateOut(other_base);
    Copy(other_base.template Cast<ForwardHttpTask>());
  }
};

}  // namespace dt_provenance::proxy

#endif  // DT_PROVENANCE_PROXY_TASKS_H_
