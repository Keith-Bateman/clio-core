#ifndef DT_PROVENANCE_PROTOCOL_STREAM_REASSEMBLY_H_
#define DT_PROVENANCE_PROTOCOL_STREAM_REASSEMBLY_H_

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "provider.h"

namespace dt_provenance::protocol {

using json = nlohmann::ordered_json;

/**
 * Callback invoked for each parsed chunk from a stream
 * @param chunk_json The parsed JSON object from one SSE data line or NDJSON line
 */
using StreamChunkCallback = std::function<void(const json& chunk_json)>;

/**
 * Reassembles SSE or NDJSON streamed responses into individual JSON chunks
 *
 * Ported from Python agent-interception proxy/streaming.py.
 *
 * SSE format (Anthropic, OpenAI):
 *   event: message_delta\n
 *   data: {"type":"message_delta",...}\n
 *   \n
 *
 * NDJSON format (Ollama):
 *   {"model":"llama3","message":{"content":"Hello"},"done":false}\n
 *   {"model":"llama3","message":{"content":" world"},"done":true}\n
 */
class StreamReassembler {
 public:
  /**
   * Construct a stream reassembler
   * @param provider Provider type (determines SSE vs NDJSON parsing)
   */
  explicit StreamReassembler(Provider provider);

  /**
   * Feed raw bytes from the HTTP response body
   * @param data Raw bytes to process
   * @param callback Invoked for each complete JSON chunk found
   */
  void Feed(std::string_view data, const StreamChunkCallback& callback);

  /**
   * Flush any remaining buffered data (call at end of stream)
   * @param callback Invoked for any final chunk
   */
  void Flush(const StreamChunkCallback& callback);

  /** Get total number of chunks parsed so far */
  size_t ChunkCount() const { return chunk_count_; }

 private:
  void ParseSSE(std::string_view data, const StreamChunkCallback& callback);
  void ParseNDJSON(std::string_view data, const StreamChunkCallback& callback);

  Provider provider_;
  std::string buffer_;
  size_t chunk_count_ = 0;
};

/**
 * Reassemble a complete SSE response body into individual JSON chunks
 * @param body Complete SSE response body
 * @return Vector of parsed JSON chunks
 */
std::vector<json> ReassembleSSE(std::string_view body);

/**
 * Reassemble a complete NDJSON response body into individual JSON chunks
 * @param body Complete NDJSON response body
 * @return Vector of parsed JSON chunks
 */
std::vector<json> ReassembleNDJSON(std::string_view body);

}  // namespace dt_provenance::protocol

#endif  // DT_PROVENANCE_PROTOCOL_STREAM_REASSEMBLY_H_
