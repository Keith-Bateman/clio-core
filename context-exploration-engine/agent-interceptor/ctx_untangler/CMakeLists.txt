cmake_minimum_required(VERSION 3.10)

add_clio_module_client(
  SOURCES
    src/ctx_untangler_client.cc
  LINK_LIBRARIES
    $<BUILD_INTERFACE:dt_provenance::protocol>
)

add_clio_module_runtime(
  SOURCES
    src/ctx_untangler_runtime.cc
    src/autogen/ctx_untangler_lib_exec.cc
  LINK_LIBRARIES
    clio_cte_core_client
    $<BUILD_INTERFACE:dt_provenance::protocol>
    $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
)
