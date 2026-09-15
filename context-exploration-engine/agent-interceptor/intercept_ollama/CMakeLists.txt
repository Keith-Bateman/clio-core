cmake_minimum_required(VERSION 3.10)

add_clio_module_client(
  SOURCES
    src/ollama_client.cc
  LINK_LIBRARIES
    $<BUILD_INTERFACE:dt_provenance::protocol>
)

add_clio_module_runtime(
  SOURCES
    src/ollama_runtime.cc
    src/autogen/intercept_ollama_lib_exec.cc
  LINK_LIBRARIES
    $<BUILD_INTERFACE:dt_provenance::protocol>
    $<BUILD_INTERFACE:dt_provenance_dt_tracker_client>
    $<BUILD_INTERFACE:httplib::httplib>
    $<BUILD_INTERFACE:OpenSSL::SSL>
    $<BUILD_INTERFACE:OpenSSL::Crypto>
    $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
)
