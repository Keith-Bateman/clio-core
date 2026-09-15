#ifndef DT_PROVENANCE_PROXY_AUTOGEN_METHODS_H_
#define DT_PROVENANCE_PROXY_AUTOGEN_METHODS_H_

#include <clio_runtime/clio_runtime.h>
#include <string>
#include <vector>

/**
 * Auto-generated method definitions for dt_proxy
 */

namespace dt_provenance::proxy {

namespace Method {
// Inherited methods
GLOBAL_CROSS_CONST clio::run::u32 kCreate = 0;
GLOBAL_CROSS_CONST clio::run::u32 kDestroy = 1;
GLOBAL_CROSS_CONST clio::run::u32 kMonitor = 9;

// dt_proxy-specific methods
GLOBAL_CROSS_CONST clio::run::u32 kForwardHttp = 10;

GLOBAL_CROSS_CONST clio::run::u32 kMaxMethodId = 11;

inline const std::vector<std::string>& GetMethodNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v(kMaxMethodId);
    v[0] = "Create";
    v[1] = "Destroy";
    v[9] = "Monitor";
    v[10] = "ForwardHttp";
    return v;
  }();
  return names;
}
}  // namespace Method

}  // namespace dt_provenance::proxy

#endif  // PROXY_AUTOGEN_METHODS_H_
