#ifndef DT_PROVENANCE_TRACKER_FAILURE_DETECTOR_H
#define DT_PROVENANCE_TRACKER_FAILURE_DETECTOR_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace dt_provenance::tracker {

/// Inspects a stored InteractionRecord JSON for failure conditions and
/// produces zero or more recovery event JSON objects ready to be written
/// to CTE under Recovery_<session_id>.
class FailureDetector {
 public:
  /// Returns one recovery event per detected failure in @p record.
  /// Returns empty if the interaction completed successfully.
  static std::vector<nlohmann::json> Detect(const nlohmann::json& record);

 private:
  static std::string GenerateEventId();
  static nlohmann::json MakeEvent(const nlohmann::json& record,
                                  const std::string& error_type,
                                  const std::string& description);
};

}  // namespace dt_provenance::tracker

#endif  // DT_PROVENANCE_TRACKER_FAILURE_DETECTOR_H
