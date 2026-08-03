add_library(
  MigrationEngine
  STATIC
    src/bundle_capture.cpp
    src/dism.cpp
    src/restore_inspection.cpp
    src/shrink_bundle.cpp
    src/source_analysis.cpp
    src/target_layout.cpp
    src/target_layout_io.cpp
    src/volume_apply.cpp
)

add_library(ytec::migration_engine ALIAS MigrationEngine)

target_include_directories(
  MigrationEngine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  MigrationEngine
  PUBLIC
    ytec::boot_repair
    ytec::disk_model
    ytec::image_format
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(MigrationEngine)
