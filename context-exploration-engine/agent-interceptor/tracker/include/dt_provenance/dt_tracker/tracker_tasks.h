#ifndef DT_PROVENANCE_TRACKER_TASKS_H_
#define DT_PROVENANCE_TRACKER_TASKS_H_

#include <clio_runtime/admin/admin_tasks.h>
#include <clio_runtime/clio_runtime.h>
#include <clio_runtime/config_manager.h>
#include <yaml-cpp/yaml.h>

#include "autogen/tracker_methods.h"

namespace dt_provenance::tracker {

using MonitorTask = clio::run::admin::MonitorTask;

/**
 * CreateParams for the Conversation Tracker ChiMod
 */
struct CreateParams {
  static constexpr const char* chimod_lib_name = "dt_provenance_dt_tracker";

  CreateParams() = default;

  template <class Archive>
  void serialize(Archive& ar) {
    // No parameters needed
    (void)ar;
  }

  void LoadConfig(const clio::run::PoolConfig& pool_config) {
    (void)pool_config;
  }
};

using CreateTask = clio::run::admin::GetOrCreatePoolTask<CreateParams>;
using DestroyTask = clio::run::admin::DestroyTask;

/**
 * StoreInteractionTask — store an interaction record in CTE
 */
struct StoreInteractionTask : public clio::run::Task {
  IN clio::run::priv::string interaction_json_;
  OUT clio::run::u64 sequence_id_;

  StoreInteractionTask()
      : clio::run::Task(), interaction_json_(CLIO_PRIV_ALLOC), sequence_id_(0) {}

  explicit StoreInteractionTask(const clio::run::TaskId& task_node,
                                const clio::run::PoolId& pool_id,
                                const clio::run::PoolQuery& pool_query,
                                const std::string& interaction_json)
      : clio::run::Task(task_node, pool_id, pool_query, 10),
        interaction_json_(CLIO_PRIV_ALLOC),
        sequence_id_(0) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kStoreInteraction;
    task_flags_.Clear();
    pool_query_ = pool_query;
    interaction_json_ = interaction_json;
  }

  template <typename Archive>
  void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(interaction_json_);
  }

  template <typename Archive>
  void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(sequence_id_);
  }

  void Copy(const ctp::ipc::FullPtr<StoreInteractionTask>& other) {
    Task::Copy(other.template Cast<Task>());
    interaction_json_ = other->interaction_json_;
    sequence_id_ = other->sequence_id_;
  }

  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task>& other_base) {
    Task::AggregateOut(other_base);
    Copy(other_base.template Cast<StoreInteractionTask>());
  }
};

/**
 * QuerySessionTask — retrieve all interactions for a session
 */
struct QuerySessionTask : public clio::run::Task {
  IN clio::run::priv::string session_id_;
  OUT clio::run::priv::string interactions_json_;  // JSON array

  QuerySessionTask()
      : clio::run::Task(),
        session_id_(CLIO_PRIV_ALLOC),
        interactions_json_(CLIO_PRIV_ALLOC) {}

  explicit QuerySessionTask(const clio::run::TaskId& task_node,
                            const clio::run::PoolId& pool_id,
                            const clio::run::PoolQuery& pool_query,
                            const std::string& session_id)
      : clio::run::Task(task_node, pool_id, pool_query, 10),
        session_id_(CLIO_PRIV_ALLOC),
        interactions_json_(CLIO_PRIV_ALLOC) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kQuerySession;
    task_flags_.Clear();
    pool_query_ = pool_query;
    session_id_ = session_id;
  }

  template <typename Archive>
  void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(session_id_);
  }

  template <typename Archive>
  void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(interactions_json_);
  }

  void Copy(const ctp::ipc::FullPtr<QuerySessionTask>& other) {
    Task::Copy(other.template Cast<Task>());
    session_id_ = other->session_id_;
    interactions_json_ = other->interactions_json_;
  }

  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task>& other_base) {
    Task::AggregateOut(other_base);
    Copy(other_base.template Cast<QuerySessionTask>());
  }
};

/**
 * ListSessionsTask — list all sessions with summary info
 */
struct ListSessionsTask : public clio::run::Task {
  OUT clio::run::priv::string sessions_json_;  // JSON array

  ListSessionsTask() : clio::run::Task(), sessions_json_(CLIO_PRIV_ALLOC) {}

  explicit ListSessionsTask(const clio::run::TaskId& task_node,
                            const clio::run::PoolId& pool_id,
                            const clio::run::PoolQuery& pool_query)
      : clio::run::Task(task_node, pool_id, pool_query, 10),
        sessions_json_(CLIO_PRIV_ALLOC) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kListSessions;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template <typename Archive>
  void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
  }

  template <typename Archive>
  void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(sessions_json_);
  }

  void Copy(const ctp::ipc::FullPtr<ListSessionsTask>& other) {
    Task::Copy(other.template Cast<Task>());
    sessions_json_ = other->sessions_json_;
  }

  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task>& other_base) {
    Task::AggregateOut(other_base);
    Copy(other_base.template Cast<ListSessionsTask>());
  }
};

/**
 * GetInteractionTask — retrieve a single interaction by session + sequence ID
 */
struct GetInteractionTask : public clio::run::Task {
  IN clio::run::priv::string session_id_;
  IN clio::run::u64 sequence_id_;
  OUT clio::run::priv::string interaction_json_;

  GetInteractionTask()
      : clio::run::Task(),
        session_id_(CLIO_PRIV_ALLOC),
        sequence_id_(0),
        interaction_json_(CLIO_PRIV_ALLOC) {}

  explicit GetInteractionTask(const clio::run::TaskId& task_node,
                              const clio::run::PoolId& pool_id,
                              const clio::run::PoolQuery& pool_query,
                              const std::string& session_id,
                              clio::run::u64 sequence_id)
      : clio::run::Task(task_node, pool_id, pool_query, 10),
        session_id_(CLIO_PRIV_ALLOC),
        sequence_id_(sequence_id),
        interaction_json_(CLIO_PRIV_ALLOC) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kGetInteraction;
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
    ar(interaction_json_);
  }

  void Copy(const ctp::ipc::FullPtr<GetInteractionTask>& other) {
    Task::Copy(other.template Cast<Task>());
    session_id_ = other->session_id_;
    sequence_id_ = other->sequence_id_;
    interaction_json_ = other->interaction_json_;
  }

  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task>& other_base) {
    Task::AggregateOut(other_base);
    Copy(other_base.template Cast<GetInteractionTask>());
  }
};

}  // namespace dt_provenance::tracker

#endif  // DT_PROVENANCE_TRACKER_TASKS_H_
