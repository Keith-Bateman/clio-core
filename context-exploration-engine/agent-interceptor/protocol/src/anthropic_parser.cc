#include "dt_provenance/protocol/anthropic_parser.h"

#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>

namespace {

struct Pricing {
  double input_per_m;
  double output_per_m;
};

// Returns the merged pricing table: hardcoded defaults overridden by
// ~/.dt_provenance/pricing.json entries (if the file exists and is valid JSON).
// Initialized once on first call (C++11 static-local guarantee).
const std::unordered_map<std::string, Pricing>& GetPricingTable() {
  static const std::unordered_map<std::string, Pricing> table = []() {
    std::unordered_map<std::string, Pricing> t = {
        // Claude 4 family
        {"claude-opus-4", {15.0, 75.0}},
        {"claude-opus-4-6", {15.0, 75.0}},
        {"claude-sonnet-4", {3.0, 15.0}},
        {"claude-sonnet-4-6", {3.0, 15.0}},
        // Claude 3.5 family
        {"claude-3-5-sonnet-20241022", {3.0, 15.0}},
        {"claude-3-5-sonnet-latest", {3.0, 15.0}},
        {"claude-3-5-haiku-20241022", {0.80, 4.0}},
        // Claude 3 family
        {"claude-3-opus-20240229", {15.0, 75.0}},
        {"claude-3-sonnet-20240229", {3.0, 15.0}},
        {"claude-3-haiku-20240307", {0.25, 1.25}},
    };
    const char* home = std::getenv("HOME");
    if (home) {
      std::ifstream f(std::string(home) + "/.dt_provenance/pricing.json");
      if (f.is_open()) {
        try {
          nlohmann::json j;
          f >> j;
          for (auto& [model, p] : j.items()) {
            if (p.contains("input_per_m") && p.contains("output_per_m")) {
              t[model] = {p["input_per_m"].get<double>(),
                          p["output_per_m"].get<double>()};
            }
          }
        } catch (...) {
        }
      }
    }
    return t;
  }();
  return table;
}

}  // namespace

namespace dt_provenance::protocol {

void AnthropicParser::ParseRequest(const json& body, InteractionRecord& record) {
  // Model
  if (body.contains("model")) {
    record.model = body["model"].get<std::string>();
  }

  // Stream flag
  record.response.is_streaming = body.value("stream", false);

  // System prompt: can be string or array of content blocks
  if (body.contains("system")) {
    const auto& sys = body["system"];
    if (sys.is_string()) {
      record.context_metrics.system_prompt_length =
          static_cast<int64_t>(sys.get<std::string>().size());
    } else if (sys.is_array()) {
      int64_t total = 0;
      for (const auto& block : sys) {
        if (block.contains("text")) {
          total += static_cast<int64_t>(block["text"].get<std::string>().size());
        }
      }
      record.context_metrics.system_prompt_length = total;
    }
  }

  // Messages: count by role
  if (body.contains("messages") && body["messages"].is_array()) {
    const auto& messages = body["messages"];
    record.context_metrics.message_count =
        static_cast<int32_t>(messages.size());
    int64_t depth = 0;

    for (const auto& msg : messages) {
      std::string role = msg.value("role", "");
      if (role == "user") {
        record.context_metrics.user_turn_count++;
      } else if (role == "assistant") {
        record.context_metrics.assistant_turn_count++;
      } else if (role == "tool") {
        record.context_metrics.tool_result_count++;
      }

      // Approximate context depth
      if (msg.contains("content")) {
        if (msg["content"].is_string()) {
          depth += static_cast<int64_t>(msg["content"].get<std::string>().size());
        } else if (msg["content"].is_array()) {
          for (const auto& block : msg["content"]) {
            if (block.contains("text")) {
              depth += static_cast<int64_t>(block["text"].get<std::string>().size());
            }
          }
        }
      }
    }
    record.context_metrics.context_depth_chars = depth;
  }

  // Store request body for reference
  record.request.body = body;
}

void AnthropicParser::ParseResponse(const json& body,
                                    InteractionRecord& record) {
  // Content blocks
  if (body.contains("content") && body["content"].is_array()) {
    std::string text;
    for (const auto& block : body["content"]) {
      std::string type = block.value("type", "");
      if (type == "text") {
        if (!text.empty()) text += "\n";
        text += block.value("text", "");
      } else if (type == "tool_use") {
        ToolCall tc;
        tc.id = block.value("id", "");
        tc.name = block.value("name", "");
        if (block.contains("input")) {
          tc.input = block["input"];
        }
        record.response.tool_calls.push_back(std::move(tc));
      }
    }
    record.response.text = std::move(text);
  }

  // Stop reason
  record.response.stop_reason = body.value("stop_reason", "");

  // Model (response may override request)
  if (body.contains("model")) {
    record.model = body["model"].get<std::string>();
  }

  // Usage
  if (body.contains("usage")) {
    const auto& usage = body["usage"];
    record.metrics.input_tokens = usage.value("input_tokens", int64_t{0});
    record.metrics.output_tokens = usage.value("output_tokens", int64_t{0});
    record.metrics.cache_creation_tokens =
        usage.value("cache_creation_input_tokens", int64_t{0});
    record.metrics.cache_read_tokens =
        usage.value("cache_read_input_tokens", int64_t{0});
  }
}

void AnthropicParser::ParseStreamChunk(const json& event_data,
                                       InteractionRecord& record) {
  std::string type = event_data.value("type", "");

  if (type == "message_start") {
    // Extract model and initial usage from message_start
    if (event_data.contains("message")) {
      const auto& msg = event_data["message"];
      if (msg.contains("model")) {
        record.model = msg["model"].get<std::string>();
      }
      if (msg.contains("usage")) {
        const auto& usage = msg["usage"];
        record.metrics.input_tokens = usage.value("input_tokens", int64_t{0});
        record.metrics.cache_creation_tokens =
            usage.value("cache_creation_input_tokens", int64_t{0});
        record.metrics.cache_read_tokens =
            usage.value("cache_read_input_tokens", int64_t{0});
      }
    }
  } else if (type == "content_block_start") {
    // Start of a new content block — may be tool_use
    if (event_data.contains("content_block")) {
      const auto& block = event_data["content_block"];
      std::string block_type = block.value("type", "");
      if (block_type == "tool_use") {
        ToolCall tc;
        tc.id = block.value("id", "");
        tc.name = block.value("name", "");
        tc.input = json::object();
        record.response.tool_calls.push_back(std::move(tc));
      }
    }
  } else if (type == "content_block_delta") {
    if (event_data.contains("delta")) {
      const auto& delta = event_data["delta"];
      std::string delta_type = delta.value("type", "");
      if (delta_type == "text_delta") {
        record.response.text += delta.value("text", "");
      } else if (delta_type == "input_json_delta") {
        // Accumulate partial JSON for tool input
        // The tool_calls vector should already have the current tool
        // We'll concatenate partial JSON — it will be re-parsed when complete
        if (!record.response.tool_calls.empty()) {
          auto& current = record.response.tool_calls.back();
          std::string partial = delta.value("partial_json", "");
          if (!partial.empty()) {
            // Store partial JSON as string in the input field
            if (current.input.is_string()) {
              current.input = current.input.get<std::string>() + partial;
            } else if (current.input.is_object() && current.input.empty()) {
              current.input = partial;
            } else {
              // Already has complete JSON; this shouldn't happen normally
              current.input = current.input.dump() + partial;
            }
          }
        }
      } else if (delta_type == "thinking_delta") {
        // Thinking blocks — append to text with marker
        // We don't treat thinking separately for storage
      }
    }
  } else if (type == "content_block_stop") {
    // Finalize tool input JSON if it was accumulated as a string
    if (!record.response.tool_calls.empty()) {
      auto& current = record.response.tool_calls.back();
      if (current.input.is_string()) {
        std::string raw = current.input.get<std::string>();
        if (!raw.empty()) {
          try {
            current.input = json::parse(raw);
          } catch (const json::parse_error&) {
            // Keep as string if parsing fails
          }
        }
      }
    }
  } else if (type == "message_delta") {
    // Final usage and stop_reason
    if (event_data.contains("delta")) {
      const auto& delta = event_data["delta"];
      if (delta.contains("stop_reason")) {
        record.response.stop_reason = delta["stop_reason"].get<std::string>();
      }
    }
    if (event_data.contains("usage")) {
      const auto& usage = event_data["usage"];
      record.metrics.output_tokens = usage.value("output_tokens", int64_t{0});
    }
  }
}

CostEstimate AnthropicParser::EstimateCost(const std::string& model,
                                           const TokenUsage& usage) {
  const auto& pricing_table = GetPricingTable();

  CostEstimate est;
  est.model = model;

  // Find pricing — try exact match, then prefix match
  auto it = pricing_table.find(model);
  if (it == pricing_table.end()) {
    for (const auto& [key, _] : pricing_table) {
      if (model.starts_with(key) || key.starts_with(model)) {
        it = pricing_table.find(key);
        break;
      }
    }
  }

  if (it != pricing_table.end()) {
    const auto& pricing = it->second;
    double effective_input =
        static_cast<double>(usage.input_tokens + usage.cache_creation_tokens) +
        static_cast<double>(usage.cache_read_tokens) * 0.1;  // 90% discount for cached
    est.input_cost = effective_input * pricing.input_per_m / 1'000'000.0;
    est.output_cost = static_cast<double>(usage.output_tokens) * pricing.output_per_m / 1'000'000.0;
    est.total_cost = est.input_cost + est.output_cost;
  } else {
    est.note = "unknown model — cost not estimated";
  }

  return est;
}

}  // namespace dt_provenance::protocol
