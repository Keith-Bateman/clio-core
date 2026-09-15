#ifndef DT_PROVENANCE_TRACKER_AUTOGEN_METHODS_H_
#define DT_PROVENANCE_TRACKER_AUTOGEN_METHODS_H_

#include <clio_runtime/clio_runtime.h>
#include <string>
#include <vector>

/**
 * Auto-generated method definitions for dt_tracker
 */

namespace dt_provenance::tracker {

namespace Method {
// Inherited methods
GLOBAL_CROSS_CONST clio::run::u32 kCreate = 0;
GLOBAL_CROSS_CONST clio::run::u32 kDestroy = 1;
GLOBAL_CROSS_CONST clio::run::u32 kMonitor = 9;

// dt_tracker-specific methods
GLOBAL_CROSS_CONST clio::run::u32 kStoreInteraction = 10;
GLOBAL_CROSS_CONST clio::run::u32 kQuerySession = 11;
GLOBAL_CROSS_CONST clio::run::u32 kListSessions = 12;
GLOBAL_CROSS_CONST clio::run::u32 kGetInteraction = 13;

GLOBAL_CROSS_CONST clio::run::u32 kMaxMethodId = 14;

inline const std::vector<std::string>& GetMethodNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v(kMaxMethodId);
    v[0] = "Create";
    v[1] = "Destroy";
    v[9] = "Monitor";
    v[10] = "StoreInteraction";
    v[11] = "QuerySession";
    v[12] = "ListSessions";
    v[13] = "GetInteraction";
    return v;
  }();
  return names;
}
}  // namespace Method

}  // namespace dt_provenance::tracker

#endif  // TRACKER_AUTOGEN_METHODS_H_
