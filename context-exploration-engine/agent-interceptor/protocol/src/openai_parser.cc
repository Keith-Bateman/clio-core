#include "dt_provenance/protocol/openai_parser.h"

#include <unordered_map>

namespace dt_provenance::protocol {

void OpenAIParser::ParseRequest(const json& body, InteractionRecord& record) {
  // Model
  if (body.contains("model")) {
    record.model = body["model"].get<std::string>();
  }

  // Stream flag
  record.response.is_streaming = body.value("stream", false);

  // Messages
  if (body.contains("messages") && body["messages"].is_array()) {
    const auto& messages = body["messages"];
    record.context_metrics.message_count =
        static_cast<int32_t>(messages.size());
    int64_t depth = 0;

    for (const auto& msg : messages) {
      std::string role = msg.value("role", "");
      if (role == "system") {
        // First system message = system prompt
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
      } else if (role == "tool") {
        record.context_metrics.tool_result_count++;
        if (msg.contains("content") && msg["content"].is_string()) {
          depth += static_cast<int64_t>(msg["content"].get<std::string>().size());
        }
      }
    }
    record.context_metrics.context_depth_chars = depth;
  }

  record.request.body = body;
}

void OpenAIParser::ParseResponse(const json& body, InteractionRecord& record) {
  // Model
  if (body.contains("model")) {
    record.model = body["model"].get<std::string>();
  }

  // Choices
  if (body.contains("choices") && body["choices"].is_array() &&
      !body["choices"].empty()) {
    const auto& choice = body["choices"][0];

    // finish_reason
    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
      record.response.stop_reason = choice["finish_reason"].get<std::string>();
    }

    // Message content
    if (choice.contains("message")) {
      const auto& msg = choice["message"];
      if (msg.contains("content") && !msg["content"].is_null()) {
        record.response.text = msg["content"].get<std::string>();
      }

      // Tool calls
      if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        for (const auto& tc : msg["tool_calls"]) {
          ToolCall tool_call;
          tool_call.id = tc.value("id", "");
          if (tc.contains("function")) {
            tool_call.name = tc["function"].value("name", "");
            std::string args_str = tc["function"].value("arguments", "");
            if (!args_str.empty()) {
              try {
                tool_call.input = json::parse(args_str);
              } catch (const json::parse_error&) {
                tool_call.input = args_str;
              }
            }
          }
          record.response.tool_calls.push_back(std::move(tool_call));
        }
      }
    }
  }

  // Usage
  if (body.contains("usage")) {
    const auto& usage = body["usage"];
    record.metrics.input_tokens = usage.value("prompt_tokens", int64_t{0});
    record.metrics.output_tokens = usage.value("completion_tokens", int64_t{0});
  }
}

void OpenAIParser::ParseStreamChunk(const json& event_data,
                                    InteractionRecord& record) {
  // Model
  if (event_data.contains("model")) {
    record.model = event_data["model"].get<std::string>();
  }

  // Choices
  if (event_data.contains("choices") && event_data["choices"].is_array() &&
      !event_data["choices"].empty()) {
    const auto& choice = event_data["choices"][0];

    // finish_reason
    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
      record.response.stop_reason = choice["finish_reason"].get<std::string>();
    }

    // Delta
    if (choice.contains("delta")) {
      const auto& delta = choice["delta"];

      // Text content
      if (delta.contains("content") && !delta["content"].is_null()) {
        record.response.text += delta["content"].get<std::string>();
      }

      // Tool calls (streaming: accumulate by index)
      if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& tc_delta : delta["tool_calls"]) {
          int index = tc_delta.value("index", 0);

          // Ensure tool_calls vector is large enough
          while (static_cast<int>(record.response.tool_calls.size()) <= index) {
            record.response.tool_calls.emplace_back();
          }

          auto& tc = record.response.tool_calls[index];
          if (tc_delta.contains("id")) {
            tc.id = tc_delta["id"].get<std::string>();
          }
          if (tc_delta.contains("function")) {
            const auto& func = tc_delta["function"];
            if (func.contains("name")) {
              tc.name = func["name"].get<std::string>();
            }
            if (func.contains("arguments")) {
              // Accumulate partial arguments JSON
              std::string partial = func["arguments"].get<std::string>();
              if (tc.input.is_string()) {
                tc.input = tc.input.get<std::string>() + partial;
              } else if (tc.input.is_null()) {
                tc.input = partial;
              }
            }
          }
        }
      }
    }
  }

  // Usage (only in final chunk when stream_options.include_usage=true)
  if (event_data.contains("usage") && !event_data["usage"].is_null()) {
    const auto& usage = event_data["usage"];
    record.metrics.input_tokens = usage.value("prompt_tokens", int64_t{0});
    record.metrics.output_tokens = usage.value("completion_tokens", int64_t{0});
  }
}

CostEstimate OpenAIParser::EstimateCost(const std::string& model,
                                        const TokenUsage& usage) {
  struct Pricing {
    double input_per_m;
    double output_per_m;
  };

  static const std::unordered_map<std::string, Pricing> pricing_table = {
      {"gpt-4o", {2.50, 10.0}},
      {"gpt-4o-2024-11-20", {2.50, 10.0}},
      {"gpt-4o-mini", {0.15, 0.60}},
      {"gpt-4-turbo", {10.0, 30.0}},
      {"gpt-4", {30.0, 60.0}},
      {"gpt-3.5-turbo", {0.50, 1.50}},
      {"o1", {15.0, 60.0}},
      {"o1-mini", {3.0, 12.0}},
      {"o3-mini", {1.10, 4.40}},
  };

  CostEstimate est;
  est.model = model;

  auto it = pricing_table.find(model);
  if (it == pricing_table.end()) {
    for (const auto& [key, _] : pricing_table) {
      if (model.starts_with(key)) {
        it = pricing_table.find(key);
        break;
      }
    }
  }

  if (it != pricing_table.end()) {
    const auto& pricing = it->second;
    est.input_cost = static_cast<double>(usage.input_tokens) * pricing.input_per_m / 1'000'000.0;
    est.output_cost = static_cast<double>(usage.output_tokens) * pricing.output_per_m / 1'000'000.0;
    est.total_cost = est.input_cost + est.output_cost;
  } else {
    est.note = "unknown model — cost not estimated";
  }

  return est;
}

}  // namespace dt_provenance::protocol
