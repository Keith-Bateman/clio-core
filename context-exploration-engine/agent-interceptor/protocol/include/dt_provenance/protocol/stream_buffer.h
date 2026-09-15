#ifndef DT_PROVENANCE_PROTOCOL_STREAM_BUFFER_H_
#define DT_PROVENANCE_PROTOCOL_STREAM_BUFFER_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dt_provenance::protocol {

/**
 * Thread-safe buffer for streaming LLM response chunks between
 * an interception ChiMod (producer) and the proxy HTTP handler (consumer).
 *
 * Producer calls: SetResponseHeaders(), PushChunk(), Complete() / SetError()
 * Consumer calls: WaitForHeaders(), PopChunk(), GetCollectedBody()
 */
class StreamBuffer {
 public:
  /** Producer: set upstream response status + headers (unblocks WaitForHeaders) */
  void SetResponseHeaders(int status, const std::string& headers_json);

  /** Consumer: block until headers are available; returns {status, headers_json} */
  std::pair<int, std::string> WaitForHeaders(int timeout_sec = 60);

  /** Producer: push a chunk of response data */
  void PushChunk(const std::string& data);

  /** Producer: signal that the stream is complete */
  void Complete();

  /** Producer: signal an error (sets headers_ready + done so consumer unblocks) */
  void SetError(int status, const std::string& error_body);

  /** Consumer: pop next chunk; blocks until available. Returns {data, is_done} */
  std::pair<std::string, bool> PopChunk(int timeout_sec = 300);

  /** Post-stream: return the full collected body for parsing */
  std::string GetCollectedBody() const;

 private:
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<std::string> chunks_;
  std::string collected_body_;
  bool done_ = false;
  int status_ = 0;
  std::string headers_json_;
  bool headers_ready_ = false;
  bool error_ = false;
};

/**
 * Global registry of active StreamBuffers, keyed by unique ID.
 * The proxy creates a buffer before dispatching; the interception
 * ChiMod looks it up by ID (passed via request_time_ns_).
 */
class StreamBufferRegistry {
 public:
  static StreamBufferRegistry& Instance();

  /** Create a new StreamBuffer; returns {id, shared_ptr} */
  std::pair<uint64_t, std::shared_ptr<StreamBuffer>> Create();

  /** Look up a buffer by ID; returns nullptr if not found */
  std::shared_ptr<StreamBuffer> Get(uint64_t id);

  /** Remove a buffer (called after stream is fully consumed) */
  void Remove(uint64_t id);

 private:
  StreamBufferRegistry() = default;
  std::mutex mtx_;
  std::unordered_map<uint64_t, std::shared_ptr<StreamBuffer>> buffers_;
  std::atomic<uint64_t> next_id_{1};
};

}  // namespace dt_provenance::protocol

#endif  // DT_PROVENANCE_PROTOCOL_STREAM_BUFFER_H_
