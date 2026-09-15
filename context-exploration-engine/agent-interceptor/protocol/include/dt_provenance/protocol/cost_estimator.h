#ifndef DT_PROVENANCE_PROTOCOL_COST_ESTIMATOR_H_
#define DT_PROVENANCE_PROTOCOL_COST_ESTIMATOR_H_

#include <string>

#include "interaction.h"
#include "provider.h"

namespace dt_provenance::protocol {

/**
 * Unified cost estimator that dispatches to provider-specific pricing
 *
 * Pricing tables ported from Python agent-interception providers/*.py
 */
class CostEstimator {
 public:
  /**
   * Estimate cost for an LLM API call
   * @param provider Provider type
   * @param model Model name string
   * @param usage Token usage from the response
   * @return CostEstimate with input/output/total cost in USD
   */
  static CostEstimate Estimate(Provider provider, const std::string& model,
                               const TokenUsage& usage);
};

}  // namespace dt_provenance::protocol

#endif  // DT_PROVENANCE_PROTOCOL_COST_ESTIMATOR_H_
