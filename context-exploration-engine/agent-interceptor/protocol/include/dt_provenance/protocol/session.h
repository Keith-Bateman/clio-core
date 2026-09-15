#ifndef DT_PROVENANCE_PROTOCOL_SESSION_H_
#define DT_PROVENANCE_PROTOCOL_SESSION_H_

#include <optional>
#include <string>
#include <string_view>

namespace dt_provenance::protocol {

/**
 * Result of session extraction from a URL path
 */
struct SessionInfo {
  std::string session_id;
  std::string stripped_path;  // Path with /_session/{id} prefix removed

  SessionInfo(std::string sid, std::string path)
      : session_id(std::move(sid)), stripped_path(std::move(path)) {}
};

/**
 * Extract session ID from a URL path
 *
 * Expected format: /_session/{session_id}/rest/of/path
 * Ported from Python agent-interception proxy/handler.py:114-123
 *
 * @param path The full request path
 * @return SessionInfo if session prefix found, nullopt otherwise
 */
std::optional<SessionInfo> ExtractSession(std::string_view path);

}  // namespace dt_provenance::protocol

#endif  // DT_PROVENANCE_PROTOCOL_SESSION_H_
