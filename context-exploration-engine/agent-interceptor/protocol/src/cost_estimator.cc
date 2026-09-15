#include "dt_provenance/protocol/cost_estimator.h"

#include "dt_provenance/protocol/anthropic_parser.h"
#include "dt_provenance/protocol/ollama_parser.h"
#include "dt_provenance/protocol/openai_parser.h"

namespace dt_provenance::protocol {

CostEstimate CostEstimator::Estimate(Provider provider,
                                     const std::string& model,
                                     const TokenUsage& usage) {
  switch (provider) {
    case Provider::kAnthropic:
      return AnthropicParser::EstimateCost(model, usage);
    case Provider::kOpenAI:
      return OpenAIParser::EstimateCost(model, usage);
    case Provider::kOllama:
      return OllamaParser::EstimateCost(model, usage);
    case Provider::kUnknown: {
      CostEstimate est;
      est.model = model;
      est.note = "unknown provider — cost not estimated";
      return est;
    }
  }
  return {};
}

}  // namespace dt_provenance::protocol
