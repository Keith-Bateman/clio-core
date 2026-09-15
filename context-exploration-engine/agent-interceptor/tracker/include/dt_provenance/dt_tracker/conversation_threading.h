#ifndef DT_PROVENANCE_TRACKER_CONVERSATION_THREADING_H_
#define DT_PROVENANCE_TRACKER_CONVERSATION_THREADING_H_

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "dt_provenance/protocol/interaction.h"

namespace dt_provenance::tracker {

using json = nlohmann::ordered_json;

/**
 * Tracks conversation threading state across interactions
 *
 * Ported from Python agent-interception storage/store.py:140-197.
 * Determines turn_type, links parent interactions, and generates
 * conversation IDs.
 */
class ConversationThreader {
 public:
  /**
   * Resolve threading information for a new interaction
   *
   * Updates the record's conversation field in-place:
   * - conversation_id: UUID or inherited from previous turn
   * - parent_sequence_id: previous interaction in this conversation
   * - turn_number: incremented from parent
   * - turn_type: initial, continuation, tool_result
   *
   * @param record The interaction record to update (must have session_id set)
   */
  void ResolveThreading(dt_provenance::protocol::InteractionRecord& record);

 private:
  /**
   * Check if this interaction is a continuation of the previous one
   * Signal 1: previous response text appears in current messages
   * Signal 2: previous had tool_calls AND current has tool_result messages
   */
  bool IsContinuation(
      const dt_provenance::protocol::InteractionRecord& current,
      const dt_provenance::protocol::InteractionRecord& previous) const;

  /** Check if the request contains tool_result messages */
  bool HasToolResults(
      const dt_provenance::protocol::InteractionRecord& record) const;

  /** Generate a simple UUID-like string */
  static std::string GenerateConversationId();

  // Per-session state: last interaction
  struct SessionState {
    uint64_t last_sequence_id = 0;
    std::string conversation_id;
    int32_t turn_number = 0;
    std::string last_response_text;
    bool last_had_tool_calls = false;
  };

  std::unordered_map<std::string, SessionState> sessions_;
};

}  // namespace dt_provenance::tracker

#endif  // DT_PROVENANCE_TRACKER_CONVERSATION_THREADING_H_
