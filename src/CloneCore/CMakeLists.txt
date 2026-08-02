add_library(
  CloneCore
  STATIC
    src/error.cpp
    src/crc32.cpp
    src/disk_identity.cpp
    src/gpt.cpp
    src/gpt_windows_guid.cpp
    src/log.cpp
    src/mbr.cpp
    src/offline_gpt_clone.cpp
    src/offline_mbr_clone.cpp
    src/windows_volume_bitmap.cpp
)

add_library(ytec::clone_core ALIAS CloneCore)

target_include_directories(
  CloneCore
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(CloneCore PRIVATE bcrypt)

ytec_configure_target(CloneCore)
