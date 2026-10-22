cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_TRANSFORM_VECTORS "Build the scene-item transform vector generator" OFF)

if(NOT ENABLE_TRANSFORM_VECTORS)
  return()
endif()

add_executable(transform-vectors transform-vectors.c)
target_link_libraries(transform-vectors PRIVATE OBS::libobs)

if(NOT MSVC)
  # obs_add_data_path is deprecated but remains the way to point a bare
  # libobs client at the effects directory of a build tree.
  target_compile_options(transform-vectors PRIVATE -Wno-deprecated-declarations)
endif()

set_target_properties_obs(transform-vectors PROPERTIES FOLDER "Tests and Examples")
