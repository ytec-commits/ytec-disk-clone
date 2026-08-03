add_library(
  ImageFormat
  STATIC
    src/backup_manifest.cpp
    src/compression.cpp
    src/dcimg.cpp
    src/image_inspection.cpp
    src/job_file.cpp
    src/job_manifest.cpp
    src/job_result.cpp
    src/partition_snapshot.cpp
    src/restore.cpp
    src/restore_image_inspection.cpp
    src/sha256.cpp
    src/shrink_image_manifest.cpp
    src/windows_file_staging.cpp
)

add_library(ytec::image_format ALIAS ImageFormat)

target_include_directories(
  ImageFormat
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  ImageFormat
  PUBLIC
    ytec::clone_core
    ytec::migration_core
  PRIVATE
    bcrypt
    advapi32
    ytec::disk_model
    ytec::zstd
)

ytec_configure_target(ImageFormat)
