cmake_minimum_required(VERSION 3.10)

add_clio_module_client(
  SOURCES
    src/proxy_client.cc
  LINK_LIBRARIES
    $<BUILD_INTERFACE:dt_provenance::protocol>
)

add_clio_module_runtime(
  SOURCES
    src/proxy_runtime.cc
    src/http_proxy_server.cc
    src/session_guard.cc
    src/autogen/proxy_lib_exec.cc
  LINK_LIBRARIES
    clio_cte_core_client
    $<BUILD_INTERFACE:dt_provenance::protocol>
    $<BUILD_INTERFACE:dt_provenance_dt_tracker_client>
    $<BUILD_INTERFACE:httplib::httplib>
    $<BUILD_INTERFACE:OpenSSL::SSL>
    $<BUILD_INTERFACE:OpenSSL::Crypto>
    $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
)
