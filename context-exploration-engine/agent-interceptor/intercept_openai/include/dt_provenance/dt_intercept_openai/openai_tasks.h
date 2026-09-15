#ifndef DT_PROVENANCE_INTERCEPTION_OPENAI_TASKS_H_
#define DT_PROVENANCE_INTERCEPTION_OPENAI_TASKS_H_

#include <clio_runtime/admin/admin_tasks.h>
#include <clio_runtime/clio_runtime.h>
#include <clio_runtime/config_manager.h>
#include <yaml-cpp/yaml.h>

#include "autogen/intercept_openai_methods.h"

namespace dt_provenance::intercept_openai {

using MonitorTask = clio::run::admin::MonitorTask;

/**
 * CreateParams for the OpenAI Interception ChiMod
 */
struct CreateParams {
  clio::run::priv::string upstream_base_url_;

  static constexpr const char* chimod_lib_name =
      "dt_provenance_dt_intercept_openai";

  CreateParams() : upstream_base_url_(CLIO_PRIV_ALLOC) {
    upstream_base_url_ = "https://api.openai.com";
  }

  explicit CreateParams(const std::string& url)
      : upstream_base_url_(CLIO_PRIV_ALLOC) {
    upstream_base_url_ = url;
  }

  template <class Archive>
  void serialize(Archive& ar) {
    ar(upstream_base_url_);
  }

  void LoadConfig(const clio::run::PoolConfig& pool_config) {
    YAML::Node node = YAML::Load(pool_config.config_);
    if (node["upstream_base_url"]) {
      upstream_base_url_ = node["upstream_base_url"].as<std::string>();
    } else if (node["config"]) {
      YAML::Node inner = YAML::Load(node["config"].as<std::string>());
      if (inner["upstream_base_url"]) {
        upstream_base_url_ = inner["upstream_base_url"].as<std::string>();
      }
    }
  }
};

using CreateTask = clio::run::admin::GetOrCreatePoolTask<CreateParams>;
using DestroyTask = clio::run::admin::DestroyTask;

/**
 * InterceptAndForwardTask — forward a request to OpenAI and capture the
 * interaction
 */
struct InterceptAndForwardTask : public clio::run::Task {
  // IN fields
  IN clio::run::priv::string session_id_;
  IN clio::run::priv::string path_;
  IN clio::run::priv::string headers_json_;
  IN clio::run::priv::string request_body_;
  IN clio::run::u64 request_time_ns_;

  // OUT fields
  OUT int32_t response_status_;
  OUT clio::run::priv::string response_headers_json_;
  OUT clio::run::priv::string response_body_;
  OUT double latency_ms_;
  OUT double ttft_ms_;

  /** SHM default constructor */
  InterceptAndForwardTask()
      : clio::run::Task(),
        session_id_(CLIO_PRIV_ALLOC),
        path_(CLIO_PRIV_ALLOC),
        headers_json_(CLIO_PRIV_ALLOC),
        request_body_(CLIO_PRIV_ALLOC),
        request_time_ns_(0),
        response_status_(0),
        response_headers_json_(CLIO_PRIV_ALLOC),
        response_body_(CLIO_PRIV_ALLOC),
        latency_ms_(0),
        ttft_ms_(0) {}

  /** Emplace constructor */
  explicit InterceptAndForwardTask(
      const clio::run::TaskId& task_node, const clio::run::PoolId& pool_id,
      const clio::run::PoolQuery& pool_query, const std::string& session_id,
      const std::string& path, const std::string& headers_json,
      const std::string& request_body, clio::run::u64 request_time_ns)
      : clio::run::Task(task_node, pool_id, pool_query, 10),
        session_id_(CLIO_PRIV_ALLOC),
        path_(CLIO_PRIV_ALLOC),
        headers_json_(CLIO_PRIV_ALLOC),
        request_body_(CLIO_PRIV_ALLOC),
        request_time_ns_(request_time_ns),
        response_status_(0),
        response_headers_json_(CLIO_PRIV_ALLOC),
        response_body_(CLIO_PRIV_ALLOC),
        latency_ms_(0),
        ttft_ms_(0) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kInterceptAndForward;
    task_flags_.Clear();
    pool_query_ = pool_query;

    session_id_ = session_id;
    path_ = path;
    headers_json_ = headers_json;
    request_body_ = request_body;
  }

  template <typename Archive>
  void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(session_id_, path_, headers_json_, request_body_, request_time_ns_);
  }

  template <typename Archive>
  void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(response_status_, response_headers_json_, response_body_, latency_ms_,
       ttft_ms_);
  }

  void Copy(const ctp::ipc::FullPtr<InterceptAndForwardTask>& other) {
    Task::Copy(other.template Cast<Task>());
    session_id_ = other->session_id_;
    path_ = other->path_;
    headers_json_ = other->headers_json_;
    request_body_ = other->request_body_;
    request_time_ns_ = other->request_time_ns_;
    response_status_ = other->response_status_;
    response_headers_json_ = other->response_headers_json_;
    response_body_ = other->response_body_;
    latency_ms_ = other->latency_ms_;
    ttft_ms_ = other->ttft_ms_;
  }

  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task>& other_base) {
    Task::AggregateOut(other_base);
    Copy(other_base.template Cast<InterceptAndForwardTask>());
  }
};

}  // namespace dt_provenance::intercept_openai

#endif  // DT_PROVENANCE_INTERCEPTION_OPENAI_TASKS_H_
