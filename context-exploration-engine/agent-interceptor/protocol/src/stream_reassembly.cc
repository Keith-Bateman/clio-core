#include "dt_provenance/protocol/stream_reassembly.h"

namespace dt_provenance::protocol {

StreamReassembler::StreamReassembler(Provider provider) : provider_(provider) {}

void StreamReassembler::Feed(std::string_view data,
                             const StreamChunkCallback& callback) {
  buffer_.append(data);

  if (provider_ == Provider::kOllama) {
    ParseNDJSON(buffer_, callback);
  } else {
    ParseSSE(buffer_, callback);
  }
}

void StreamReassembler::Flush(const StreamChunkCallback& callback) {
  if (!buffer_.empty()) {
    // Try to parse any remaining data
    if (provider_ == Provider::kOllama) {
      ParseNDJSON(buffer_, callback);
    } else {
      ParseSSE(buffer_, callback);
    }
    buffer_.clear();
  }
}

void StreamReassembler::ParseSSE(std::string_view data,
                                 const StreamChunkCallback& callback) {
  // SSE format: lines separated by \n
  // "data: {json}\n\n" marks a complete event
  // Skip "event:", "id:", "retry:" lines
  std::string remaining;
  size_t pos = 0;

  while (pos < buffer_.size()) {
    // Find the next line
    auto newline = buffer_.find('\n', pos);
    if (newline == std::string::npos) {
      // Incomplete line — keep in buffer
      remaining = buffer_.substr(pos);
      break;
    }

    std::string_view line(buffer_.data() + pos, newline - pos);
    // Strip trailing \r
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    pos = newline + 1;

    // Empty line marks end of an event (we process data lines individually)
    if (line.empty()) {
      continue;
    }

    // Only process "data: " lines
    if (!line.starts_with("data: ") && !line.starts_with("data:")) {
      continue;
    }

    // Extract data payload
    std::string_view payload;
    if (line.starts_with("data: ")) {
      payload = line.substr(6);
    } else {
      payload = line.substr(5);
    }

    // Skip "[DONE]" marker (OpenAI convention)
    if (payload == "[DONE]") {
      continue;
    }

    // Try to parse as JSON
    if (!payload.empty()) {
      try {
        auto chunk = json::parse(payload);
        chunk_count_++;
        callback(chunk);
      } catch (const json::parse_error&) {
        // Malformed chunk — skip
      }
    }
  }

  buffer_ = std::move(remaining);
}

void StreamReassembler::ParseNDJSON(std::string_view /*data*/,
                                    const StreamChunkCallback& callback) {
  // NDJSON: each line is a complete JSON object
  std::string remaining;
  size_t pos = 0;

  while (pos < buffer_.size()) {
    auto newline = buffer_.find('\n', pos);
    if (newline == std::string::npos) {
      // Incomplete line — keep in buffer
      remaining = buffer_.substr(pos);
      break;
    }

    std::string_view line(buffer_.data() + pos, newline - pos);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    pos = newline + 1;

    if (line.empty()) {
      continue;
    }

    try {
      auto chunk = json::parse(line);
      chunk_count_++;
      callback(chunk);
    } catch (const json::parse_error&) {
      // Malformed line — skip
    }
  }

  buffer_ = std::move(remaining);
}

std::vector<json> ReassembleSSE(std::string_view body) {
  std::vector<json> chunks;
  StreamReassembler reassembler(Provider::kAnthropic);
  reassembler.Feed(body, [&](const json& chunk) { chunks.push_back(chunk); });
  reassembler.Flush([&](const json& chunk) { chunks.push_back(chunk); });
  return chunks;
}

std::vector<json> ReassembleNDJSON(std::string_view body) {
  std::vector<json> chunks;
  StreamReassembler reassembler(Provider::kOllama);
  reassembler.Feed(body, [&](const json& chunk) { chunks.push_back(chunk); });
  reassembler.Flush([&](const json& chunk) { chunks.push_back(chunk); });
  return chunks;
}

}  // namespace dt_provenance::protocol
