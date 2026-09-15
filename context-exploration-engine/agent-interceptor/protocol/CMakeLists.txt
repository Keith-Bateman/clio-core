cmake_minimum_required(VERSION 3.20)

#------------------------------------------------------------------------------
# dt_protocol — Standalone protocol library for DTProvenance
#
# No Chimaera dependency. Usable by any C++ application that needs
# provider detection, session extraction, and interaction record types.
#
# Target: libdt_protocol.so
# Alias:  dt_provenance::protocol
#------------------------------------------------------------------------------

add_library(dt_protocol SHARED
  src/provider.cc
  src/session.cc
  src/interaction.cc
  src/anthropic_parser.cc
  src/openai_parser.cc
  src/ollama_parser.cc
  src/stream_reassembly.cc
  src/cost_estimator.cc
  src/stream_buffer.cc
)

add_library(dt_provenance::protocol ALIAS dt_protocol)

target_include_directories(dt_protocol
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

find_package(Threads REQUIRED)

target_link_libraries(dt_protocol
  PUBLIC
    $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
    Threads::Threads
)

set_target_properties(dt_protocol PROPERTIES
  VERSION 0.1.0
  SOVERSION 0
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)

