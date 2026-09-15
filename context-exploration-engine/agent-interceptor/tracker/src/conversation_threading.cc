#include "dt_provenance/dt_tracker/conversation_threading.h"

#include <chrono>
#include <random>
#include <sstream>

namespace dt_provenance::tracker {

using namespace dt_provenance::protocol;

std::string ConversationThreader::GenerateConversationId() {
  // Simple pseudo-UUID using random bytes
  static thread_local std::mt19937 rng(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

  std::ostringstream oss;
  oss << std::hex << dist(rng) << "-" << dist(rng) << "-" << dist(rng);
  return oss.str();
}

void ConversationThreader::ResolveThreading(InteractionRecord& record) {
  const auto& session_id = record.session_id;

  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    // First interaction in this session — start new conversation
    SessionState state;
    state.last_sequence_id = record.sequence_id;
    state.conversation_id = GenerateConversationId();
    state.turn_number = 1;
    state.last_response_text = record.response.text;
    state.last_had_tool_calls = !record.response.tool_calls.empty();

    record.conversation.conversation_id = state.conversation_id;
    record.conversation.parent_sequence_id = 0;
    record.conversation.turn_number = 1;
    record.conversation.turn_type = "initial";

    sessions_[session_id] = std::move(state);
    return;
  }

  auto& state = it->second;

  // Build a "previous" record from state for continuation check
  InteractionRecord prev;
  prev.response.text = state.last_response_text;
  if (state.last_had_tool_calls) {
    prev.response.tool_calls.push_back(ToolCall{});  // Sentinel
  }

  if (IsContinuation(record, prev)) {
    // Continuation of existing conversation
    record.conversation.conversation_id = state.conversation_id;
    record.conversation.parent_sequence_id = state.last_sequence_id;
    state.turn_number++;
    record.conversation.turn_number = state.turn_number;

    // Determine turn type
    if (HasToolResults(record)) {
      record.conversation.turn_type = "tool_result";
    } else {
      record.conversation.turn_type = "continuation";
    }
  } else {
    // New conversation in this session
    state.conversation_id = GenerateConversationId();
    state.turn_number = 1;

    record.conversation.conversation_id = state.conversation_id;
    record.conversation.parent_sequence_id = 0;
    record.conversation.turn_number = 1;
    record.conversation.turn_type = "initial";
  }

  // Update state
  state.last_sequence_id = record.sequence_id;
  state.last_response_text = record.response.text;
  state.last_had_tool_calls = !record.response.tool_calls.empty();
}

bool ConversationThreader::IsContinuation(
    const InteractionRecord& current,
    const InteractionRecord& previous) const {
  // Signal 1: previous response text appears in current messages
  if (!previous.response.text.empty() &&
      current.request.body.contains("messages")) {
    std::string prev_prefix =
        previous.response.text.substr(0, std::min(size_t{100},
                                                   previous.response.text.size()));
    for (const auto& msg : current.request.body["messages"]) {
      if (msg.value("role", "") == "assistant") {
        std::string content;
        if (msg.contains("content") && msg["content"].is_string()) {
          content = msg["content"].get<std::string>();
        } else if (msg.contains("content") && msg["content"].is_array()) {
          for (const auto& block : msg["content"]) {
            if (block.contains("text")) {
              content += block["text"].get<std::string>();
            }
          }
        }
        if (!content.empty() && content.find(prev_prefix) != std::string::npos) {
          return true;
        }
      }
    }
  }

  // Signal 2: previous had tool_calls AND current has tool_results
  if (!previous.response.tool_calls.empty() && HasToolResults(current)) {
    return true;
  }

  return false;
}

bool ConversationThreader::HasToolResults(
    const InteractionRecord& record) const {
  if (!record.request.body.contains("messages")) return false;

  for (const auto& msg : record.request.body["messages"]) {
    std::string role = msg.value("role", "");
    if (role == "tool" || role == "tool_result") {
      return true;
    }
  }
  return false;
}

}  // namespace dt_provenance::tracker
