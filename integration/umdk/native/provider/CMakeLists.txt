cmake_minimum_required(VERSION 3.12)
project(openurma_vdev_provider C)

if(NOT DEFINED UMDK_STAGE)
  message(FATAL_ERROR "UMDK_STAGE must name the frozen Q2 stage")
endif()

add_library(urma_openurma_vdev SHARED openurma_vdev_provider.c)
target_include_directories(urma_openurma_vdev PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include" "${UMDK_STAGE}/include")
target_link_libraries(urma_openurma_vdev PRIVATE "${UMDK_STAGE}/lib/liburma.so")
target_compile_options(urma_openurma_vdev PRIVATE -Wall -Wextra -Werror)
set_target_properties(urma_openurma_vdev PROPERTIES
  OUTPUT_NAME "urma_openurma_vdev"
  BUILD_RPATH "$ORIGIN/.."
  INSTALL_RPATH "$ORIGIN/..")

# Negative-only provider. It is kept in the build tree and must never be
# installed into the formal Q3 stage.
add_library(urma_wrong_vdev_provider SHARED openurma_vdev_provider.c)
target_compile_definitions(urma_wrong_vdev_provider PRIVATE
  OPENURMA_VDEV_PROVIDER_NAME="wrong_vdev_provider")
target_include_directories(urma_wrong_vdev_provider PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include" "${UMDK_STAGE}/include")
target_link_libraries(urma_wrong_vdev_provider PRIVATE "${UMDK_STAGE}/lib/liburma.so")
target_compile_options(urma_wrong_vdev_provider PRIVATE -Wall -Wextra -Werror)
set_target_properties(urma_wrong_vdev_provider PROPERTIES
  OUTPUT_NAME "urma_wrong_vdev_provider"
  BUILD_RPATH "$ORIGIN/.."
  INSTALL_RPATH "$ORIGIN/..")
