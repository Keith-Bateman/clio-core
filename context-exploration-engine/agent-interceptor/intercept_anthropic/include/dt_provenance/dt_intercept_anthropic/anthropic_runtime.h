#ifndef DT_PROVENANCE_INTERCEPTION_ANTHROPIC_RUNTIME_H_
#define DT_PROVENANCE_INTERCEPTION_ANTHROPIC_RUNTIME_H_

#include <clio_runtime/clio_runtime.h>
#include <memory>
#include <string>

#include "anthropic_client.h"
#include "anthropic_tasks.h"
#include "autogen/intercept_anthropic_methods.h"
#include "dt_provenance/dt_tracker/tracker_client.h"

namespace httplib {
class SSLClient;
class Client;
}  // namespace httplib

namespace dt_provenance::tracker { class Client; }

namespace dt_provenance::intercept_anthropic {

/**
 * Runtime container for the Anthropic Interception ChiMod
 *
 * Owns an httplib HTTPS client pointing at api.anthropic.com.
 * Handles InterceptAndForward: forward → receive → parse → dispatch to tracker.
 */
class Runtime : public clio::run::Container {
 public:
  using CreateParams = dt_provenance::intercept_anthropic::CreateParams;

  Runtime() = default;
  ~Runtime() override;

  // Container interface
  void Init(const clio::run::PoolId& pool_id, const std::string& pool_name,
            clio::run::u32 container_id = 0) override;
  clio::run::TaskResume Run(clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> task_ptr) override;

  // Method handlers
  clio::run::TaskResume Create(clio::run::shared_ptr<CreateTask> &task);
  clio::run::TaskResume InterceptAndForward(
      clio::run::shared_ptr<InterceptAndForwardTask> &task);
  clio::run::TaskResume Monitor(clio::run::shared_ptr<MonitorTask> &task);
  clio::run::TaskResume Destroy(clio::run::shared_ptr<DestroyTask> &task);

  // Container virtual methods
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
  Client client_;
  std::string upstream_host_;
  int upstream_port_ = 443;
  bool upstream_ssl_ = true;
  std::atomic<uint64_t> active_requests_{0};

  // Tracker dispatch (lazy-init)
  std::unique_ptr<dt_provenance::tracker::Client> tracker_client_;
  bool tracker_initialized_ = false;
};

}  // namespace dt_provenance::intercept_anthropic

#endif  // DT_PROVENANCE_INTERCEPTION_ANTHROPIC_RUNTIME_H_
