cmake_minimum_required(VERSION 3.20)

#------------------------------------------------------------------------------
# ctx_writer — CTE verification tool
#------------------------------------------------------------------------------

add_executable(ctx_writer ctx_writer.cc)

target_link_libraries(ctx_writer PRIVATE
  clio_run_cxx
  dt_provenance_dt_tracker_client
  dt_provenance::protocol
  $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
)

set_target_properties(ctx_writer PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)
