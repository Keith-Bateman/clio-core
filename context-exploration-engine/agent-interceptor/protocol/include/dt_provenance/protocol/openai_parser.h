#ifndef DT_PROVENANCE_PROTOCOL_OPENAI_PARSER_H_
#define DT_PROVENANCE_PROTOCOL_OPENAI_PARSER_H_

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "interaction.h"

namespace dt_provenance::protocol {

using json = nlohmann::ordered_json;

/**
 * Parser for OpenAI Chat Completions API request/response formats
 *
 * Ported from Python agent-interception providers/openai.py.
 * Handles both streaming (SSE) and non-streaming responses.
 */
class OpenAIParser {
 public:
  /**
   * Parse an OpenAI API request body
   * @param body Parsed JSON request body
   * @param record InteractionRecord to populate
   */
  static void ParseRequest(const json& body, InteractionRecord& record);

  /**
   * Parse a non-streaming OpenAI API response body
   * @param body Parsed JSON response body
   * @param record InteractionRecord to populate
   */
  static void ParseResponse(const json& body, InteractionRecord& record);

  /**
   * Parse a single SSE data chunk from a streaming response
   * @param event_data The JSON data from a "data: " SSE line
   * @param record InteractionRecord to accumulate into
   */
  static void ParseStreamChunk(const json& event_data, InteractionRecord& record);

  /**
   * Estimate cost for an OpenAI API call
   * @param model Model name (e.g., "gpt-4o")
   * @param usage Token usage from the response
   * @return CostEstimate with input/output/total cost in USD
   */
  static CostEstimate EstimateCost(const std::string& model,
                                   const TokenUsage& usage);
};

}  // namespace dt_provenance::protocol

#endif  // DT_PROVENANCE_PROTOCOL_OPENAI_PARSER_H_
