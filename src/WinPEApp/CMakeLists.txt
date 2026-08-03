add_library(
  WinPEAppLogic
  STATIC
    src/app_runner.cpp
    src/clone_execution_readiness.cpp
    src/clone_execution_service.cpp
    src/dashboard.cpp
    src/job_manifest_candidate_provider.cpp
    src/job_manifest_loader.cpp
    src/job_result.cpp
    src/mbr2gpt_job_execution_service.cpp
    src/product_io_policy.cpp
    src/repair_layout.cpp
    src/restore_execution_readiness.cpp
    src/restore_execution_service.cpp
    src/restore_execution_safety_probe.cpp
    src/restore_image_candidate_provider.cpp
    src/restore_image_verifier.cpp
    src/shrink_execution_service.cpp
    src/winre_diagnostic_view.cpp
)

add_library(ytec::winpe_app_logic ALIAS WinPEAppLogic)

target_include_directories(
  WinPEAppLogic
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  WinPEAppLogic
  PUBLIC
    ytec::boot_repair
    ytec::cli_tools
    ytec::image_format
    ytec::migration_engine
  PRIVATE
    Advapi32
    Ole32
    Uuid
)

ytec_configure_target(WinPEAppLogic)

target_compile_definitions(
  WinPEAppLogic
  PUBLIC
    YTEC_PROJECT_VERSION="${PROJECT_VERSION}"
)

add_executable(
  ytec-winpe-app
  src/main.cpp
)

target_link_libraries(
  ytec-winpe-app
  PRIVATE
    ytec::winpe_app_logic
)

target_compile_definitions(
  ytec-winpe-app
  PRIVATE
    YTEC_WINPE_PRODUCT_BOUNDARY=1
)

ytec_configure_target(ytec-winpe-app)

add_executable(
  ytec-winpe-gui
  WIN32
    src/gui_main.cpp
    ${YTEC_LINE_SEED_FONT_RESOURCE}
)

target_link_libraries(
  ytec-winpe-gui
  PRIVATE
    ytec::winpe_app_logic
    ytec::ui_support
)

target_compile_definitions(
  ytec-winpe-gui
  PRIVATE
    UNICODE
    _UNICODE
    YTEC_WINPE_PRODUCT_BOUNDARY=1
)

ytec_configure_target(ytec-winpe-gui)
