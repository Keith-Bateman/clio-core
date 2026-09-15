#ifndef DT_PROVENANCE_PROTOCOL_PROVIDER_H_
#define DT_PROVENANCE_PROTOCOL_PROVIDER_H_

#include <string>
#include <string_view>
#include <unordered_map>

namespace dt_provenance::protocol {

/**
 * Supported LLM provider types
 */
enum class Provider {
  kAnthropic,
  kOpenAI,
  kOllama,
  kUnknown
};

/**
 * Result of provider detection
 */
struct ProviderInfo {
  Provider provider;
  std::string upstream_base_url;

  ProviderInfo() : provider(Provider::kUnknown) {}
  ProviderInfo(Provider p, std::string url)
      : provider(p), upstream_base_url(std::move(url)) {}
};

/**
 * Convert provider enum to string
 * @param provider The provider enum value
 * @return Human-readable provider name
 */
std::string ProviderToString(Provider provider);

/**
 * Parse provider from string
 * @param name Provider name string
 * @return Provider enum value (kUnknown if not recognized)
 */
Provider ProviderFromString(std::string_view name);

/**
 * Detect LLM provider from request path and headers
 *
 * Detection logic (matches Python agent-interception registry.py):
 * - /v1/messages → Anthropic
 * - /api/* → Ollama
 * - /v1/* + anthropic-version header → Anthropic
 * - /v1/* (fallback) → OpenAI
 * - /_interceptor/* → Unknown (internal route)
 *
 * @param path Request path (after session prefix stripping)
 * @param headers Request headers (key-value pairs)
 * @return ProviderInfo with detected provider and default upstream URL
 */
ProviderInfo DetectProvider(
    std::string_view path,
    const std::unordered_map<std::string, std::string>& headers);

/**
 * Get default upstream base URL for a provider
 * @param provider The provider type
 * @return Default HTTPS base URL for the provider
 */
std::string GetDefaultUpstreamUrl(Provider provider);

}  // namespace dt_provenance::protocol

#endif  // DT_PROVENANCE_PROTOCOL_PROVIDER_H_
