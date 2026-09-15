#ifndef DT_PROVENANCE_PROXY_RUNTIME_H_
#define DT_PROVENANCE_PROXY_RUNTIME_H_

#include <clio_runtime/clio_runtime.h>

#include <atomic>
#include <chrono>
#include <memory>

#include "proxy_client.h"
#include "proxy_tasks.h"
#include "autogen/proxy_methods.h"

namespace dt_provenance::tracker { class Client; }

namespace dt_provenance::proxy {

/**
 * Runtime container for the HTTP Proxy ChiMod
 *
 * All LLM forwarding now happens on Chimaera worker threads via the
 * Monitor handler.  A Python Flask bridge translates agent HTTP requests
 * into pool_stats://800.0:local:<json> queries that land here.
 */
class Runtime : public clio::run::Container {
 public:
  using CreateParams = dt_provenance::proxy::CreateParams;

  Runtime() = default;
  ~Runtime() override;

  // Container interface
  void Init(const clio::run::PoolId& pool_id, const std::string& pool_name,
            clio::run::u32 container_id = 0) override;
  clio::run::TaskResume Run(clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> task_ptr) override;

  // Method handlers
  clio::run::TaskResume Create(clio::run::shared_ptr<CreateTask> &task);
  clio::run::TaskResume Monitor(clio::run::shared_ptr<MonitorTask> &task);
  clio::run::TaskResume Destroy(clio::run::shared_ptr<DestroyTask> &task);
  clio::run::TaskResume ForwardHttp(clio::run::shared_ptr<ForwardHttpTask> &task);

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
  void HandleDispatchStats(clio::run::shared_ptr<MonitorTask>& task);
  void HandleOverheadLogging(clio::run::shared_ptr<MonitorTask>& task,
                              const std::string& query_str);
  clio::run::TaskResume HandleForwardAction(clio::run::shared_ptr<MonitorTask> task,
                                      const std::string& query_json);
  clio::run::TaskResume HandleRecordOnly(clio::run::shared_ptr<MonitorTask> task,
                                   const std::string& query_json);
  clio::run::TaskResume HandleListSessions(clio::run::shared_ptr<MonitorTask> task);
  clio::run::TaskResume HandleQuerySession(clio::run::shared_ptr<MonitorTask> task,
                                     const std::string& session_id);
  clio::run::TaskResume HandleGetInteraction(clio::run::shared_ptr<MonitorTask> task,
                                       const std::string& body);
  clio::run::TaskResume HandleListGraphs(clio::run::shared_ptr<MonitorTask> task);
  clio::run::TaskResume HandleQueryGraph(clio::run::shared_ptr<MonitorTask> task,
                                   const std::string& body);
  clio::run::TaskResume HandleGetNode(clio::run::shared_ptr<MonitorTask> task,
                                const std::string& body);
  clio::run::TaskResume HandleStoreRecoveryEvent(clio::run::shared_ptr<MonitorTask> task,
                                           const std::string& json_payload);
  clio::run::TaskResume HandleQueryRecoveryEvents(clio::run::shared_ptr<MonitorTask> task,
                                            const std::string& session_id);
  clio::run::TaskResume HandleAckRecoveryEvent(clio::run::shared_ptr<MonitorTask> task,
                                         const std::string& body);
  clio::run::TaskResume HandleStoreLgCheckpoint(clio::run::shared_ptr<MonitorTask> task,
                                          const std::string& body);
  clio::run::TaskResume HandleQueryLgCheckpoints(clio::run::shared_ptr<MonitorTask> task,
                                           const std::string& tag_name);
  clio::run::TaskResume HandleListCheckpointSessions(clio::run::shared_ptr<MonitorTask> task);
  clio::run::TaskResume HandleStoreCheckpoint(clio::run::shared_ptr<MonitorTask> task,
                                        const std::string& body);
  clio::run::TaskResume HandleQueryCheckpoints(clio::run::shared_ptr<MonitorTask> task,
                                         const std::string& session_id);

  bool EnsureTrackerClient();

  Client client_;

  // Tracker client (lazy-initialized on first use)
  bool tracker_initialized_ = false;
  std::unique_ptr<tracker::Client> tracker_client_;

  // Dispatch stats
  std::atomic<uint64_t> total_requests_{0};
  std::chrono::steady_clock::time_point start_time_;

  // Overhead stats (microseconds for precision)
  std::atomic<uint64_t> total_proxy_overhead_us_{0};    // BuildInteractionRecord time
  std::atomic<uint64_t> total_pipeline_overhead_us_{0}; // proxy + tracker + untangler

  // Runtime toggle — when false, all overhead timing is skipped
  std::atomic<bool> overhead_logging_{true};
};

}  // namespace dt_provenance::proxy

#endif  // DT_PROVENANCE_PROXY_RUNTIME_H_
