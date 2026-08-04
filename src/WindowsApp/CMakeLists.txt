add_library(
  WindowsAppLogic
  STATIC
    src/job_creation.cpp
    src/job_result_import.cpp
    src/layout.cpp
    src/media_creation.cpp
    src/media_preflight.cpp
    src/media_wizard.cpp
    src/online_backup_job.cpp
    src/online_shrink_backup_job.cpp
    src/progress.cpp
    src/reboot_handoff.cpp
    src/restore_preflight.cpp
    src/selection.cpp
    src/system_state.cpp
    src/usb_volume_mapping.cpp
)

add_library(ytec::windows_app ALIAS WindowsAppLogic)

target_include_directories(
  WindowsAppLogic
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  WindowsAppLogic
  PUBLIC
    ytec::clone_core
    ytec::disk_model
    ytec::image_format
    ytec::media_builder
    ytec::migration_engine
    ytec::vss_requester
  PRIVATE
    advapi32
    ole32
    ytec::boot_repair
    wuguid
)

set(
  YTEC_MEDIA_BUILDER_SCRIPT
  "${PROJECT_SOURCE_DIR}/scripts/New-WinPEAppValidationMedia.ps1"
)
file(
  SHA256
  "${YTEC_MEDIA_BUILDER_SCRIPT}"
  YTEC_MEDIA_BUILDER_SHA256
)
file(
  SIZE
  "${YTEC_MEDIA_BUILDER_SCRIPT}"
  YTEC_MEDIA_BUILDER_SIZE
)
target_compile_definitions(
  WindowsAppLogic
  PRIVATE
    YTEC_MEDIA_BUILDER_SHA256="${YTEC_MEDIA_BUILDER_SHA256}"
    YTEC_MEDIA_BUILDER_SIZE=${YTEC_MEDIA_BUILDER_SIZE}
)

ytec_configure_target(WindowsAppLogic)

add_executable(
  ytec-tsumugi-drive
  WIN32
    src/main.cpp
    src/windows_app.manifest
    src/windows_app.rc
    ${YTEC_LINE_SEED_FONT_RESOURCE}
)

target_link_libraries(
  ytec-tsumugi-drive
  PRIVATE
    ytec::windows_app
    ytec::ui_support
    comdlg32
    comctl32
    dwmapi
    ntdll
    shell32
    shcore
    uxtheme
)

target_compile_definitions(
  ytec-tsumugi-drive
  PRIVATE
    UNICODE
    _UNICODE
)

ytec_configure_target(ytec-tsumugi-drive)
