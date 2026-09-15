cmake_minimum_required(VERSION 3.20)

#------------------------------------------------------------------------------
# dt_demo_server — DTProvenance launcher
#------------------------------------------------------------------------------

add_executable(dt_demo_server dt_demo_server.cc)

target_link_libraries(dt_demo_server PRIVATE
  clio_run_cxx
)

set_target_properties(dt_demo_server PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)

# Copy compose config to build directory
configure_file(dt_provenance_server.yaml ${CMAKE_CURRENT_BINARY_DIR}/dt_provenance_server.yaml COPYONLY)
