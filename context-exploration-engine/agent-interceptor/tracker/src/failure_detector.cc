#include "dt_provenance/dt_tracker/failure_detector.h"

#include <chrono>
#include <random>
#include <sstream>

namespace dt_provenance::tracker {

std::string FailureDetector::GenerateEventId() {
  static thread_local std::mt19937 rng(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
  std::ostringstream oss;
  oss << std::hex << dist(rng) << "-" << dist(rng) << "-" << dist(rng);
  return oss.str();
}

nlohmann::json FailureDetector::MakeEvent(const nlohmann::json& record,
                                           const std::string& error_type,
                                           const std::string& description) {
  std::string session_id = record.value("session_id", "unknown");
  uint64_t seq_id = record.value("sequence_id", uint64_t{0});
  std::string timestamp = record.value("timestamp", "");

  uint64_t suggest_seq = 0;
  if (record.contains("conversation") &&
      record["conversation"].contains("parent_sequence_id")) {
    suggest_seq = record["conversation"].value("parent_sequence_id", uint64_t{0});
  }

  return nlohmann::json{
    {"event_id",             GenerateEventId()},
    {"event_type",           "error_report"},
    {"source",               "auto_detector"},
    {"source_session_id",    session_id},
    {"source_sequence_id",   seq_id},
    {"target_session_id",    session_id},
    {"timestamp",            timestamp},
    {"acknowledged",         false},
    {"payload", {
      {"error_type",                  error_type},
      {"description",                 description},
      {"model",                       record.value("model", "")},
      {"status_code",                 record.value("status_code", 0)},
      {"stop_reason",                 record.value("stop_reason", "")},
      {"suggest_restore_to_sequence", suggest_seq}
    }}
  };
}

std::vector<nlohmann::json> FailureDetector::Detect(const nlohmann::json& record) {
  std::vector<nlohmann::json> events;

  int status_code = record.value("status_code", 200);
  std::string stop_reason = record.value("stop_reason", "");

  // HTTP-level failures (excluding 429 — agent can't receive the message anyway)
  if (status_code == 401 || status_code == 403) {
    events.push_back(MakeEvent(record, "auth_error",
        "Authentication/authorization failure (HTTP " +
        std::to_string(status_code) + ")."));
  } else if (status_code >= 500) {
    events.push_back(MakeEvent(record, "provider_error",
        "Provider server error (HTTP " + std::to_string(status_code) + ")."));
  } else if (status_code >= 400) {
    events.push_back(MakeEvent(record, "client_error",
        "Client error (HTTP " + std::to_string(status_code) + ")."));
  }

  // Model-level failures
  if (stop_reason == "max_tokens") {
    events.push_back(MakeEvent(record, "context_exhausted",
        "Response truncated: max_tokens reached. Context may need pruning."));
  } else if (stop_reason == "error") {
    events.push_back(MakeEvent(record, "model_error",
        "Model returned stop_reason=error."));
  }

  return events;
}

}  // namespace dt_provenance::tracker
