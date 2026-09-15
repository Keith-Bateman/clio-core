#include "dt_provenance/protocol/stream_buffer.h"

#include <chrono>

namespace dt_provenance::protocol {

// ---------------------------------------------------------------------------
// StreamBuffer
// ---------------------------------------------------------------------------

void StreamBuffer::SetResponseHeaders(int status,
                                      const std::string& headers_json) {
  std::lock_guard<std::mutex> lk(mtx_);
  status_ = status;
  headers_json_ = headers_json;
  headers_ready_ = true;
  cv_.notify_all();
}

std::pair<int, std::string> StreamBuffer::WaitForHeaders(int timeout_sec) {
  std::unique_lock<std::mutex> lk(mtx_);
  cv_.wait_for(lk, std::chrono::seconds(timeout_sec),
               [this] { return headers_ready_; });
  if (!headers_ready_) {
    return {504, R"({"error":"timeout waiting for upstream headers"})"};
  }
  return {status_, headers_json_};
}

void StreamBuffer::PushChunk(const std::string& data) {
  std::lock_guard<std::mutex> lk(mtx_);
  chunks_.push_back(data);
  collected_body_.append(data);
  cv_.notify_all();
}

void StreamBuffer::Complete() {
  std::lock_guard<std::mutex> lk(mtx_);
  done_ = true;
  cv_.notify_all();
}

void StreamBuffer::SetError(int status, const std::string& error_body) {
  std::lock_guard<std::mutex> lk(mtx_);
  error_ = true;
  status_ = status;
  headers_json_ = "{}";
  headers_ready_ = true;
  // Push the error body as the only chunk so the consumer can read it
  chunks_.push_back(error_body);
  collected_body_.append(error_body);
  done_ = true;
  cv_.notify_all();
}

std::pair<std::string, bool> StreamBuffer::PopChunk(int timeout_sec) {
  std::unique_lock<std::mutex> lk(mtx_);
  cv_.wait_for(lk, std::chrono::seconds(timeout_sec),
               [this] { return !chunks_.empty() || done_; });

  if (!chunks_.empty()) {
    std::string chunk = std::move(chunks_.front());
    chunks_.pop_front();
    // Return done only if this was the last chunk AND producer is done
    bool is_done = chunks_.empty() && done_;
    return {chunk, is_done};
  }

  // No chunks and done_ is true (or timeout) — signal completion
  return {"", true};
}

std::string StreamBuffer::GetCollectedBody() const {
  std::lock_guard<std::mutex> lk(mtx_);
  return collected_body_;
}

// ---------------------------------------------------------------------------
// StreamBufferRegistry
// ---------------------------------------------------------------------------

StreamBufferRegistry& StreamBufferRegistry::Instance() {
  static StreamBufferRegistry instance;
  return instance;
}

std::pair<uint64_t, std::shared_ptr<StreamBuffer>>
StreamBufferRegistry::Create() {
  auto buf = std::make_shared<StreamBuffer>();
  uint64_t id = next_id_.fetch_add(1);
  std::lock_guard<std::mutex> lk(mtx_);
  buffers_[id] = buf;
  return {id, buf};
}

std::shared_ptr<StreamBuffer> StreamBufferRegistry::Get(uint64_t id) {
  std::lock_guard<std::mutex> lk(mtx_);
  auto it = buffers_.find(id);
  if (it != buffers_.end()) return it->second;
  return nullptr;
}

void StreamBufferRegistry::Remove(uint64_t id) {
  std::lock_guard<std::mutex> lk(mtx_);
  buffers_.erase(id);
}

}  // namespace dt_provenance::protocol
