add_library(
  VssRequester
  STATIC
    src/workflow.cpp
    src/snapshot_reader.cpp
    src/snapshot_image.cpp
    src/snapshot_copy.cpp
    src/snapshot_metadata.cpp
    src/snapshot_plan.cpp
    src/online_backup.cpp
    src/windows_backend.cpp
)

add_library(ytec::vss_requester ALIAS VssRequester)

target_include_directories(
  VssRequester
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  VssRequester
  PUBLIC
    ytec::clone_core
    ytec::image_format
  PRIVATE
    Ole32
    OleAut32
    VssApi
)

ytec_configure_target(VssRequester)
