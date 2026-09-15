/**
 * ctx_writer — CTE verification tool for DTProvenance
 *
 * Connects as a CLIO client, queries the Conversation Tracker,
 * and verifies stored data is correct and complete.
 *
 * Usage:
 *   ./ctx_writer --expected-sessions 3 \
 *                --expected-tag-prefix "Agentic_session_" \
 *                --min-interactions-per-session 1
 *
 * Exit codes:
 *   0 = all assertions passed
 *   1 = verification failure
 *
 * Ported from clio-core-dtio-old/context-exploration-engine/agent-interceptor
 * (chimaera -> clio rename).
 */

#include <clio_runtime/clio_runtime.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "dt_provenance/dt_tracker/tracker_client.h"

using json = nlohmann::ordered_json;

struct Config {
  int expected_sessions = 0;
  std::string tag_prefix = "Agentic_session_";
  int min_interactions_per_session = 1;
};

Config ParseArgs(int argc, char* argv[]) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--expected-sessions" && i + 1 < argc) {
      cfg.expected_sessions = std::stoi(argv[++i]);
    } else if (arg == "--expected-tag-prefix" && i + 1 < argc) {
      cfg.tag_prefix = argv[++i];
    } else if (arg == "--min-interactions-per-session" && i + 1 < argc) {
      cfg.min_interactions_per_session = std::stoi(argv[++i]);
    }
  }
  return cfg;
}

int main(int argc, char* argv[]) {
  Config cfg = ParseArgs(argc, argv);

  // Initialize as a CLIO client
  clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, true);

  // Connect to the tracker
  dt_provenance::tracker::Client tracker;
  clio::run::PoolId tracker_pool_id(810, 0);
  tracker.Init(tracker_pool_id);

  bool all_passed = true;

  // 1. List all sessions
  auto list_future =
      tracker.AsyncListSessions(clio::run::PoolQuery::Local());
  list_future.Wait();
  std::string sessions_str(list_future->sessions_json_.str());
  auto sessions = json::parse(sessions_str);

  std::cout << "=== DTProvenance Verification ===" << std::endl;
  std::cout << "Sessions found: " << sessions.size() << std::endl;

  if (cfg.expected_sessions > 0 &&
      static_cast<int>(sessions.size()) != cfg.expected_sessions) {
    std::cerr << "FAIL: Expected " << cfg.expected_sessions
              << " sessions, got " << sessions.size() << std::endl;
    all_passed = false;
  }

  // 2. Verify each session
  for (const auto& session_info : sessions) {
    std::string session_id = session_info["session_id"].get<std::string>();
    int count = session_info["count"].get<int>();
    std::string tag_name = session_info["tag_name"].get<std::string>();

    std::cout << "\nSession: " << session_id << " (" << count
              << " interactions, tag=" << tag_name << ")" << std::endl;

    // Verify tag prefix
    if (!tag_name.starts_with(cfg.tag_prefix)) {
      std::cerr << "  FAIL: Tag '" << tag_name << "' doesn't start with '"
                << cfg.tag_prefix << "'" << std::endl;
      all_passed = false;
    }

    // Verify minimum interactions
    if (count < cfg.min_interactions_per_session) {
      std::cerr << "  FAIL: Expected at least "
                << cfg.min_interactions_per_session
                << " interactions, got " << count << std::endl;
      all_passed = false;
    }

    // 3. Query session and verify ordering
    auto query_future =
        tracker.AsyncQuerySession(clio::run::PoolQuery::Local(), session_id);
    query_future.Wait();
    std::string interactions_str(query_future->interactions_json_.str());
    auto interactions = json::parse(interactions_str);

    uint64_t prev_seq = 0;
    for (const auto& interaction : interactions) {
      uint64_t seq = interaction["sequence_id"].get<uint64_t>();
      std::string model = interaction.value("model", "unknown");
      int in_tokens = 0;
      int out_tokens = 0;

      if (interaction.contains("metrics")) {
        in_tokens = interaction["metrics"].value("input_tokens", 0);
        out_tokens = interaction["metrics"].value("output_tokens", 0);
      }

      std::cout << "  [" << seq << "] model=" << model
                << " in=" << in_tokens << " out=" << out_tokens << std::endl;

      // Verify monotonic ordering
      if (seq <= prev_seq) {
        std::cerr << "  FAIL: Sequence ID " << seq << " <= previous "
                  << prev_seq << " (not monotonic)" << std::endl;
        all_passed = false;
      }
      prev_seq = seq;

      // Verify required fields exist
      if (!interaction.contains("session_id")) {
        std::cerr << "  FAIL: Missing session_id in interaction " << seq
                  << std::endl;
        all_passed = false;
      }
      if (!interaction.contains("provider")) {
        std::cerr << "  FAIL: Missing provider in interaction " << seq
                  << std::endl;
        all_passed = false;
      }
    }
  }

  std::cout << "\n=== Result: " << (all_passed ? "PASS" : "FAIL")
            << " ===" << std::endl;

  return all_passed ? 0 : 1;
}
