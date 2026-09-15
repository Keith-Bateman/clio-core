#ifndef DT_PROVENANCE_PROTOCOL_OLLAMA_PARSER_H_
#define DT_PROVENANCE_PROTOCOL_OLLAMA_PARSER_H_

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "interaction.h"

namespace dt_provenance::protocol {

using json = nlohmann::ordered_json;

/**
 * Parser for Ollama API request/response formats
 *
 * Ported from Python agent-interception providers/ollama.py.
 * Handles both /api/chat and /api/generate endpoints.
 * Uses NDJSON streaming (not SSE).
 */
class OllamaParser {
 public:
  /**
   * Parse an Ollama API request body
   * @param body Parsed JSON request body
   * @param path Request path (/api/chat or /api/generate)
   * @param record InteractionRecord to populate
   */
  static void ParseRequest(const json& body, std::string_view path,
                           InteractionRecord& record);

  /**
   * Parse a non-streaming Ollama API response body
   * @param body Parsed JSON response body
   * @param path Request path (/api/chat or /api/generate)
   * @param record InteractionRecord to populate
   */
  static void ParseResponse(const json& body, std::string_view path,
                            InteractionRecord& record);

  /**
   * Parse a single NDJSON line from a streaming response
   * @param line_data The parsed JSON from one NDJSON line
   * @param path Request path (/api/chat or /api/generate)
   * @param record InteractionRecord to accumulate into
   */
  static void ParseStreamChunk(const json& line_data, std::string_view path,
                               InteractionRecord& record);

  /**
   * Estimate cost for an Ollama API call (always zero — local model)
   * @param model Model name
   * @param usage Token usage
   * @return CostEstimate with zero cost
   */
  static CostEstimate EstimateCost(const std::string& model,
                                   const TokenUsage& usage);
};

}  // namespace dt_provenance::protocol

#endif  // DT_PROVENANCE_PROTOCOL_OLLAMA_PARSER_H_
