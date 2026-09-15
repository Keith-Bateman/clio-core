#include "dt_provenance/protocol/session.h"

namespace dt_provenance::protocol {

std::optional<SessionInfo> ExtractSession(std::string_view path) {
  // Expected format: /_session/{session_id}/rest/of/path
  // Split on '/' and check structure

  if (!path.starts_with("/_session/")) {
    return std::nullopt;
  }

  // Skip "/_session/" prefix (10 chars)
  std::string_view remainder = path.substr(10);

  if (remainder.empty()) {
    return std::nullopt;
  }

  // Find the next '/' to delimit the session ID
  auto slash_pos = remainder.find('/');
  std::string session_id;
  std::string stripped_path;

  if (slash_pos == std::string_view::npos) {
    // No trailing path: /_session/{id}
    session_id = std::string(remainder);
    stripped_path = "/";
  } else {
    // Has trailing path: /_session/{id}/rest/of/path
    session_id = std::string(remainder.substr(0, slash_pos));
    stripped_path = std::string(remainder.substr(slash_pos));
  }

  if (session_id.empty()) {
    return std::nullopt;
  }

  return SessionInfo(std::move(session_id), std::move(stripped_path));
}

}  // namespace dt_provenance::protocol
