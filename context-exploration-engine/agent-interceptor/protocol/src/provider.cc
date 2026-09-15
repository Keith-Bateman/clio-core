#include "dt_provenance/protocol/provider.h"

#include <algorithm>

namespace dt_provenance::protocol {

std::string ProviderToString(Provider provider) {
  switch (provider) {
    case Provider::kAnthropic: return "anthropic";
    case Provider::kOpenAI: return "openai";
    case Provider::kOllama: return "ollama";
    case Provider::kUnknown: return "unknown";
  }
  return "unknown";
}

Provider ProviderFromString(std::string_view name) {
  // Convert to lowercase for case-insensitive comparison
  std::string lower(name);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (lower == "anthropic") return Provider::kAnthropic;
  if (lower == "openai") return Provider::kOpenAI;
  if (lower == "ollama") return Provider::kOllama;
  return Provider::kUnknown;
}

std::string GetDefaultUpstreamUrl(Provider provider) {
  switch (provider) {
    case Provider::kAnthropic: return "https://api.anthropic.com";
    case Provider::kOpenAI: return "https://api.openai.com";
    case Provider::kOllama: return "http://localhost:11434";
    case Provider::kUnknown: return "";
  }
  return "";
}

ProviderInfo DetectProvider(
    std::string_view path,
    const std::unordered_map<std::string, std::string>& headers) {
  // Internal interceptor routes
  if (path.starts_with("/_interceptor")) {
    return {Provider::kUnknown, ""};
  }

  // Ollama: /api/* paths
  if (path.starts_with("/api/")) {
    return {Provider::kOllama, GetDefaultUpstreamUrl(Provider::kOllama)};
  }

  // Anthropic: /v1/messages specifically
  if (path == "/v1/messages" || path.starts_with("/v1/messages?")) {
    return {Provider::kAnthropic, GetDefaultUpstreamUrl(Provider::kAnthropic)};
  }

  // Check for anthropic-version header on any /v1/* path
  if (path.starts_with("/v1/")) {
    auto it = headers.find("anthropic-version");
    if (it != headers.end()) {
      return {Provider::kAnthropic, GetDefaultUpstreamUrl(Provider::kAnthropic)};
    }
    // Fallback: /v1/* without anthropic-version → OpenAI
    return {Provider::kOpenAI, GetDefaultUpstreamUrl(Provider::kOpenAI)};
  }

  return {Provider::kUnknown, ""};
}

}  // namespace dt_provenance::protocol
