add_library(
  MigrationCore
  STATIC
    src/direct_clone_plan.cpp
    src/shrink_layout.cpp
)

add_library(ytec::migration_core ALIAS MigrationCore)

target_include_directories(
  MigrationCore
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  MigrationCore
  PUBLIC
    ytec::clone_core
)

ytec_configure_target(MigrationCore)
