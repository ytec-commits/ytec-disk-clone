add_library(
  WinPEAppLogic
  STATIC
    src/active_rescue_media.cpp
    src/app_runner.cpp
    src/automatic_boot_repair_ui.cpp
    src/clone_execution_readiness.cpp
    src/clone_execution_service.cpp
    src/dashboard.cpp
    src/direct_image_create.cpp
    src/direct_image_restore.cpp
    src/direct_shrink_image_restore.cpp
    src/direct_image_restore_resume.cpp
    src/direct_image_restore_resume_backend.cpp
    src/direct_image_restore_resume_storage.cpp
    src/image_create_ui.cpp
    src/image_restore_ui.cpp
    src/mbr2gpt_direct_execution_service.cpp
    src/product_io_policy.cpp
    src/repair_layout.cpp
    src/rescue_clone.cpp
    src/tsumugi_password_ui.cpp
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
    ytec::disk_model
    ytec::image_format
    ytec::operation_core
    ytec::windows_shrink_restore
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
    src/winpe_gui.manifest
    src/winpe_gui.rc
    ${YTEC_LINE_SEED_FONT_RESOURCE}
)

target_link_libraries(
  ytec-winpe-gui
  PRIVATE
    Comdlg32
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

# Host-side UI acceptance build.  The product GUI source is unchanged except
# that physical-disk inventory is compiled out for this non-elevated target.
add_executable(
  ytec-winpe-gui-ui-acceptance
  WIN32
    src/gui_main.cpp
    src/winpe_gui.manifest
    src/winpe_gui.rc
    ${YTEC_LINE_SEED_FONT_RESOURCE}
)

target_link_libraries(
  ytec-winpe-gui-ui-acceptance
  PRIVATE
    Comdlg32
    ytec::winpe_app_logic
    ytec::ui_support
)

target_compile_definitions(
  ytec-winpe-gui-ui-acceptance
  PRIVATE
    UNICODE
    _UNICODE
    YTEC_WINPE_PRODUCT_BOUNDARY=1
    YTEC_UI_ACCEPTANCE_BUILD=1
)

ytec_configure_target(ytec-winpe-gui-ui-acceptance)
