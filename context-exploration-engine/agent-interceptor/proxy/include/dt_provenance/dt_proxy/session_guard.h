#ifndef DT_PROVENANCE_PROXY_SESSION_GUARD_H_
#define DT_PROVENANCE_PROXY_SESSION_GUARD_H_

#include <string>

#include "dt_provenance/protocol/provider.h"

namespace dt_provenance::proxy {

/**
 * Generate a provider-appropriate fake LLM response when session ID is missing
 *
 * Ported from Python agent-interception proxy/fake_responses.py.
 * Returns a response that looks like a valid LLM response explaining
 * that the session prefix is required.
 *
 * @param provider Detected provider type
 * @return Pair of {content_type, response_body} for the HTTP response
 */
std::pair<std::string, std::string> BuildSessionRejection(
    dt_provenance::protocol::Provider provider);

}  // namespace dt_provenance::proxy

#endif  // DT_PROVENANCE_PROXY_SESSION_GUARD_H_
