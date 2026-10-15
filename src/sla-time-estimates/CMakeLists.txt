cmake_minimum_required(VERSION 3.13)

project(sla_time_estimates)

add_library(sla_time_estimate STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/SLATimeEstimate.cpp")
target_include_directories(sla_time_estimate PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(sla_time_estimate PUBLIC nlohmann_json)
