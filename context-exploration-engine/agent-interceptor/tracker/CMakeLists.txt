cmake_minimum_required(VERSION 3.10)

add_clio_module_client(
  SOURCES
    src/tracker_client.cc
  LINK_LIBRARIES
    $<BUILD_INTERFACE:dt_provenance::protocol>
)

add_clio_module_runtime(
  SOURCES
    src/tracker_runtime.cc
    src/conversation_threading.cc
    src/failure_detector.cc
    src/autogen/tracker_lib_exec.cc
  LINK_LIBRARIES
    clio_cte_core_client
    $<BUILD_INTERFACE:dt_provenance::protocol>
    $<BUILD_INTERFACE:dt_provenance_dt_ctx_untangler_client>
    $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
)
