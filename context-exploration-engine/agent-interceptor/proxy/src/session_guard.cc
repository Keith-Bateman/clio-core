#include "dt_provenance/dt_proxy/session_guard.h"

#include <nlohmann/json.hpp>

namespace dt_provenance::proxy {

using json = nlohmann::ordered_json;

static const std::string kSessionMessage =
    "This endpoint requires a session prefix in the URL. "
    "Set ANTHROPIC_BASE_URL=http://host:port/_session/{your_session_id} "
    "to route through the DTProvenance proxy.";

std::pair<std::string, std::string> BuildSessionRejection(
    dt_provenance::protocol::Provider provider) {
  std::string content_type = "application/json";

  switch (provider) {
    case dt_provenance::protocol::Provider::kAnthropic: {
      // Anthropic message format
      json resp = {
          {"id", "msg_session_required"},
          {"type", "message"},
          {"role", "assistant"},
          {"model", "dt-provenance-proxy"},
          {"content",
           json::array({json{{"type", "text"}, {"text", kSessionMessage}}})},
          {"stop_reason", "end_turn"},
          {"usage",
           {{"input_tokens", 0}, {"output_tokens", 0}}}};
      return {content_type, resp.dump()};
    }

    case dt_provenance::protocol::Provider::kOpenAI: {
      // OpenAI chat completion format
      json resp = {
          {"id", "chatcmpl-session-required"},
          {"object", "chat.completion"},
          {"model", "dt-provenance-proxy"},
          {"choices",
           json::array(
               {json{{"index", 0},
                     {"message",
                      {{"role", "assistant"}, {"content", kSessionMessage}}},
                     {"finish_reason", "stop"}}})},
          {"usage",
           {{"prompt_tokens", 0},
            {"completion_tokens", 0},
            {"total_tokens", 0}}}};
      return {content_type, resp.dump()};
    }

    case dt_provenance::protocol::Provider::kOllama: {
      // Ollama chat response format
      json resp = {{"model", "dt-provenance-proxy"},
                   {"message",
                    {{"role", "assistant"}, {"content", kSessionMessage}}},
                   {"done", true}};
      return {content_type, resp.dump()};
    }

    default: {
      // Generic JSON error
      json resp = {{"error",
                    {{"code", -32000},
                     {"message", kSessionMessage}}}};
      return {content_type, resp.dump()};
    }
  }
}

}  // namespace dt_provenance::proxy
