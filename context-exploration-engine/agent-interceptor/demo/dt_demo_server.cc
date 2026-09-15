/**
 * DTProvenance Demo Server
 *
 * Initializes the CLIO runtime in server mode and loads the compose
 * configuration which starts all DTProvenance ChiMods:
 * - HTTP Proxy (pool 800, port 9090)
 * - Anthropic/OpenAI/Ollama Interception (pools 801-803)
 * - Conversation Tracker (pool 810)
 * - Context Untangler (pool 820)
 *
 * Ported from clio-core-dtio-old/context-exploration-engine/agent-interceptor
 * (chimaera -> clio rename).
 */

#include <clio_runtime/clio_runtime.h>
#include <csignal>
#include <iostream>
#include <thread>

static volatile bool running = true;

void SignalHandler(int sig) {
  (void)sig;
  running = false;
}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  // Reads CLIO_SERVER_CONF, starts the runtime, and creates all ChiMods
  // specified in the compose section.
  clio::run::CLIO_INIT(clio::run::RuntimeMode::kServer, true);

  std::cout << "DTProvenance server started. Press Ctrl+C to stop."
            << std::endl;

  while (running) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::cout << "DTProvenance server shutting down..." << std::endl;
  return 0;
}
