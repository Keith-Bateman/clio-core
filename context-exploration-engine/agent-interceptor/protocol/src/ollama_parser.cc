#include "dt_provenance/protocol/ollama_parser.h"

namespace dt_provenance::protocol {

void OllamaParser::ParseRequest(const json& body, std::string_view path,
                                InteractionRecord& record) {
  // Model
  if (body.contains("model")) {
    record.model = body["model"].get<std::string>();
  }

  // Stream flag (Ollama defaults to true)
  record.response.is_streaming = body.value("stream", true);

  if (path.starts_with("/api/chat")) {
    // /api/chat endpoint
    if (body.contains("messages") && body["messages"].is_array()) {
      const auto& messages = body["messages"];
      record.context_metrics.message_count =
          static_cast<int32_t>(messages.size());
      int64_t depth = 0;

      for (const auto& msg : messages) {
        std::string role = msg.value("role", "");
        if (role == "system") {
          if (msg.contains("content") && msg["content"].is_string()) {
            auto content = msg["content"].get<std::string>();
            record.context_metrics.system_prompt_length =
                static_cast<int64_t>(content.size());
            depth += static_cast<int64_t>(content.size());
          }
        } else if (role == "user") {
          record.context_metrics.user_turn_count++;
          if (msg.contains("content") && msg["content"].is_string()) {
            depth += static_cast<int64_t>(msg["content"].get<std::string>().size());
          }
        } else if (role == "assistant") {
          record.context_metrics.assistant_turn_count++;
          if (msg.contains("content") && msg["content"].is_string()) {
            depth += static_cast<int64_t>(msg["content"].get<std::string>().size());
          }
        }
      }
      record.context_metrics.context_depth_chars = depth;
    }

    // System prompt can also be a top-level field
    if (body.contains("system") && body["system"].is_string()) {
      record.context_metrics.system_prompt_length =
          static_cast<int64_t>(body["system"].get<std::string>().size());
    }
  } else if (path.starts_with("/api/generate")) {
    // /api/generate endpoint — prompt is a single string
    if (body.contains("prompt") && body["prompt"].is_string()) {
      record.context_metrics.message_count = 1;
      record.context_metrics.user_turn_count = 1;
      record.context_metrics.context_depth_chars =
          static_cast<int64_t>(body["prompt"].get<std::string>().size());
    }
    if (body.contains("system") && body["system"].is_string()) {
      record.context_metrics.system_prompt_length =
          static_cast<int64_t>(body["system"].get<std::string>().size());
    }
  }

  record.request.body = body;
}

void OllamaParser::ParseResponse(const json& body, std::string_view path,
                                 InteractionRecord& record) {
  if (path.starts_with("/api/chat")) {
    // /api/chat response
    if (body.contains("message")) {
      const auto& msg = body["message"];
      if (msg.contains("content")) {
        record.response.text = msg["content"].get<std::string>();
      }
      // Tool calls
      if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        for (const auto& tc : msg["tool_calls"]) {
          ToolCall tool_call;
          if (tc.contains("function")) {
            tool_call.name = tc["function"].value("name", "");
            if (tc["function"].contains("arguments")) {
              tool_call.input = tc["function"]["arguments"];
            }
          }
          record.response.tool_calls.push_back(std::move(tool_call));
        }
      }
    }
  } else if (path.starts_with("/api/generate")) {
    // /api/generate response
    if (body.contains("response")) {
      record.response.text = body["response"].get<std::string>();
    }
  }

  // Token counts (common to both endpoints)
  if (body.contains("prompt_eval_count")) {
    record.metrics.input_tokens = body["prompt_eval_count"].get<int64_t>();
  }
  if (body.contains("eval_count")) {
    record.metrics.output_tokens = body["eval_count"].get<int64_t>();
  }

  // Stop reason
  if (body.contains("done") && body["done"].get<bool>()) {
    record.response.stop_reason = body.value("done_reason", "stop");
  }
}

void OllamaParser::ParseStreamChunk(const json& line_data,
                                    std::string_view path,
                                    InteractionRecord& record) {
  if (path.starts_with("/api/chat")) {
    if (line_data.contains("message")) {
      const auto& msg = line_data["message"];
      if (msg.contains("content") && msg["content"].is_string()) {
        record.response.text += msg["content"].get<std::string>();
      }
    }
  } else if (path.starts_with("/api/generate")) {
    if (line_data.contains("response") && line_data["response"].is_string()) {
      record.response.text += line_data["response"].get<std::string>();
    }
  }

  // Final chunk has done=true and token counts
  if (line_data.contains("done") && line_data["done"].get<bool>()) {
    if (line_data.contains("prompt_eval_count")) {
      record.metrics.input_tokens = line_data["prompt_eval_count"].get<int64_t>();
    }
    if (line_data.contains("eval_count")) {
      record.metrics.output_tokens = line_data["eval_count"].get<int64_t>();
    }
    record.response.stop_reason = line_data.value("done_reason", "stop");
  }
}

CostEstimate OllamaParser::EstimateCost(const std::string& model,
                                        const TokenUsage& usage) {
  // Ollama runs locally — always zero cost
  CostEstimate est;
  est.model = model;
  est.input_cost = 0.0;
  est.output_cost = 0.0;
  est.total_cost = 0.0;
  est.note = "local model — no cost";
  return est;
}

}  // namespace dt_provenance::protocol
