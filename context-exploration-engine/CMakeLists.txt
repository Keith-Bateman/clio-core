cmake_minimum_required(VERSION 3.20)
project(context_exploration_engine)

# Set root directory for this component
set(CLIO_CEE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Add api subdirectory for ContextInterface C++ wrapper and Python bindings
message(STATUS "Context Exploration Engine: Building API components")
add_subdirectory(api)

# Agent-interceptor: LLM tool-call interception/tracking ChiMods (ported from
# clio-core-dtio-old). Gated on CLIO_CORE_ENABLE_CEE like the rest of CEE.
if(CLIO_CORE_ENABLE_CEE)
  message(STATUS "Context Exploration Engine: Building agent-interceptor")
  add_subdirectory(agent-interceptor)
endif()

# hdf-compass and mcp-hdf-demo are standalone components
message(STATUS "  hdf-compass and mcp-hdf-demo are standalone components")
message(STATUS "  To build them, navigate to their respective directories")
