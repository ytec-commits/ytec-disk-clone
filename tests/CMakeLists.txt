add_executable(
  ytec-unit-tests
  Unit/unit_tests.cpp
)

target_link_libraries(
  ytec-unit-tests
  PRIVATE
    ytec::clone_core
    ytec::disk_model
)

ytec_configure_target(ytec-unit-tests)
add_test(NAME ytec-unit-tests COMMAND ytec-unit-tests)

add_executable(
  ytec-shrink-layout-tests
  Unit/shrink_layout_tests.cpp
)

target_link_libraries(
  ytec-shrink-layout-tests
  PRIVATE
    ytec::migration_core
)

ytec_configure_target(ytec-shrink-layout-tests)
add_test(NAME ytec-shrink-layout-tests COMMAND ytec-shrink-layout-tests)

add_executable(
  ytec-windows-app-progress-tests
  Unit/windows_app_progress_tests.cpp
)

target_link_libraries(
  ytec-windows-app-progress-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-app-progress-tests)
add_test(
  NAME ytec-windows-app-progress-tests
  COMMAND ytec-windows-app-progress-tests
)

add_executable(
  ytec-windows-app-layout-tests
  Unit/windows_app_layout_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/WindowsApp/src/windows_app.rc
)

target_link_libraries(
  ytec-windows-app-layout-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-app-layout-tests)
add_test(
  NAME ytec-windows-app-layout-tests
  COMMAND ytec-windows-app-layout-tests
)

add_executable(
  ytec-ui-support-font-tests
  Unit/ui_support_font_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/UiSupport/resources/line_seed_fonts.rc
)

target_link_libraries(
  ytec-ui-support-font-tests
  PRIVATE
    ytec::ui_support
)

ytec_configure_target(ytec-ui-support-font-tests)
add_test(
  NAME ytec-ui-support-font-tests
  COMMAND ytec-ui-support-font-tests
)

add_executable(
  ytec-windows-online-backup-job-tests
  Unit/windows_online_backup_job_tests.cpp
)

target_link_libraries(
  ytec-windows-online-backup-job-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-online-backup-job-tests)
add_test(
  NAME ytec-windows-online-backup-job-tests
  COMMAND ytec-windows-online-backup-job-tests
)

add_executable(
  ytec-windows-system-state-tests
  Unit/windows_system_state_tests.cpp
)

target_link_libraries(
  ytec-windows-system-state-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-system-state-tests)
add_test(
  NAME ytec-windows-system-state-tests
  COMMAND ytec-windows-system-state-tests
)

add_executable(
  ytec-job-result-import-tests
  Unit/job_result_import_tests.cpp
)

target_link_libraries(
  ytec-job-result-import-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-job-result-import-tests)
add_test(
  NAME ytec-job-result-import-tests
  COMMAND ytec-job-result-import-tests
)

add_executable(
  ytec-media-preflight-tests
  Unit/media_preflight_tests.cpp
)

target_link_libraries(
  ytec-media-preflight-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-media-preflight-tests)
add_test(
  NAME ytec-media-preflight-tests
  COMMAND ytec-media-preflight-tests
)

add_executable(
  ytec-media-wizard-tests
  Unit/media_wizard_tests.cpp
)

target_link_libraries(
  ytec-media-wizard-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-media-wizard-tests)
add_test(
  NAME ytec-media-wizard-tests
  COMMAND ytec-media-wizard-tests
)

add_executable(
  ytec-usb-volume-mapping-tests
  Unit/usb_volume_mapping_tests.cpp
)

target_link_libraries(
  ytec-usb-volume-mapping-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-usb-volume-mapping-tests)
add_test(
  NAME ytec-usb-volume-mapping-tests
  COMMAND ytec-usb-volume-mapping-tests
)

add_executable(
  ytec-restore-preflight-tests
  Unit/restore_preflight_tests.cpp
)

target_link_libraries(
  ytec-restore-preflight-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-restore-preflight-tests)
add_test(
  NAME ytec-restore-preflight-tests
  COMMAND ytec-restore-preflight-tests
)

add_executable(
  ytec-mock-tests
  Integration/mock_inventory_tests.cpp
)

target_link_libraries(
  ytec-mock-tests
  PRIVATE
    ytec::cli_tools
)

ytec_configure_target(ytec-mock-tests)
add_test(NAME ytec-mock-tests COMMAND ytec-mock-tests)

add_test(NAME ytec-winpe-app-help COMMAND ytec-winpe-app --help)
set_tests_properties(
  ytec-winpe-app-help
  PROPERTIES
    PASS_REGULAR_EXPRESSION "ytec-winpe-app"
)

add_executable(
  ytec-winpe-app-tests
  Unit/winpe_app_tests.cpp
)

target_link_libraries(
  ytec-winpe-app-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-app-tests)
add_test(NAME ytec-winpe-app-tests COMMAND ytec-winpe-app-tests)

add_executable(
  ytec-winpe-dashboard-tests
  Unit/winpe_dashboard_tests.cpp
)

target_link_libraries(
  ytec-winpe-dashboard-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-dashboard-tests)
add_test(NAME ytec-winpe-dashboard-tests COMMAND ytec-winpe-dashboard-tests)

add_executable(
  ytec-phase1-synthetic-tests
  Integration/synthetic_gpt_clone_tests.cpp
)

target_link_libraries(
  ytec-phase1-synthetic-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-phase1-synthetic-tests)
add_test(NAME ytec-phase1-synthetic-tests COMMAND ytec-phase1-synthetic-tests)

add_executable(
  ytec-boot-repair-tests
  Unit/boot_repair_tests.cpp
)

target_link_libraries(
  ytec-boot-repair-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-boot-repair-tests)
add_test(NAME ytec-boot-repair-tests COMMAND ytec-boot-repair-tests)

add_executable(
  ytec-mbr2gpt-tests
  Unit/mbr2gpt_tests.cpp
)

target_link_libraries(
  ytec-mbr2gpt-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-mbr2gpt-tests)
add_test(NAME ytec-mbr2gpt-tests COMMAND ytec-mbr2gpt-tests)

add_executable(
  ytec-mbr2gpt-layout-tests
  Unit/mbr2gpt_layout_tests.cpp
)

target_link_libraries(
  ytec-mbr2gpt-layout-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-mbr2gpt-layout-tests)
add_test(
  NAME ytec-mbr2gpt-layout-tests
  COMMAND ytec-mbr2gpt-layout-tests
)

add_executable(
  ytec-mbr2gpt-execution-tests
  Unit/mbr2gpt_execution_tests.cpp
)

target_link_libraries(
  ytec-mbr2gpt-execution-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-mbr2gpt-execution-tests)
add_test(
  NAME ytec-mbr2gpt-execution-tests
  COMMAND ytec-mbr2gpt-execution-tests
)

add_executable(
  ytec-winre-diagnostic-tests
  Unit/winre_diagnostic_tests.cpp
)

target_link_libraries(
  ytec-winre-diagnostic-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-winre-diagnostic-tests)
add_test(
  NAME ytec-winre-diagnostic-tests
  COMMAND ytec-winre-diagnostic-tests
)

add_executable(
  ytec-vss-workflow-tests
  Unit/vss_workflow_tests.cpp
)

target_link_libraries(
  ytec-vss-workflow-tests
  PRIVATE
    ytec::vss_requester
)

ytec_configure_target(ytec-vss-workflow-tests)
add_test(NAME ytec-vss-workflow-tests COMMAND ytec-vss-workflow-tests)

add_executable(
  ytec-windows-bcdboot-trust-tests
  Integration/windows_bcdboot_trust_tests.cpp
)

target_link_libraries(
  ytec-windows-bcdboot-trust-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-windows-bcdboot-trust-tests)
add_test(
  NAME ytec-windows-bcdboot-trust-tests
  COMMAND ytec-windows-bcdboot-trust-tests
)

add_executable(
  ytec-media-builder-tests
  Unit/media_builder_tests.cpp
)

target_link_libraries(
  ytec-media-builder-tests
  PRIVATE
    ytec::media_builder
)

ytec_configure_target(ytec-media-builder-tests)
add_test(NAME ytec-media-builder-tests COMMAND ytec-media-builder-tests)

add_executable(
  ytec-windows-adk-detection-tests
  Integration/windows_adk_detection_tests.cpp
)

target_link_libraries(
  ytec-windows-adk-detection-tests
  PRIVATE
    ytec::media_builder
)

ytec_configure_target(ytec-windows-adk-detection-tests)
add_test(
  NAME ytec-windows-adk-detection-tests
  COMMAND ytec-windows-adk-detection-tests
)

add_test(
  NAME ytec-winpe-environment-json
  COMMAND ytec-winpe-environment --json
)
set_tests_properties(
  ytec-winpe-environment-json
  PROPERTIES
    PASS_REGULAR_EXPRESSION "\\\"schemaVersion\\\": 1"
)

add_executable(
  ytec-physical-disk-tests
  Unit/physical_disk_tests.cpp
)

target_link_libraries(
  ytec-physical-disk-tests
  PRIVATE
    ytec::disk_model
)

ytec_configure_target(ytec-physical-disk-tests)
add_test(NAME ytec-physical-disk-tests COMMAND ytec-physical-disk-tests)

add_executable(
  ytec-image-format-tests
  Unit/image_format_tests.cpp
)

target_link_libraries(
  ytec-image-format-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-image-format-tests)
add_test(NAME ytec-image-format-tests COMMAND ytec-image-format-tests)

add_executable(
  ytec-backup-manifest-tests
  Unit/backup_manifest_tests.cpp
)

target_link_libraries(
  ytec-backup-manifest-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-backup-manifest-tests)
add_test(
  NAME ytec-backup-manifest-tests
  COMMAND ytec-backup-manifest-tests
)

add_executable(
  ytec-shrink-image-manifest-tests
  Unit/shrink_image_manifest_tests.cpp
)

target_link_libraries(
  ytec-shrink-image-manifest-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-shrink-image-manifest-tests)
add_test(
  NAME ytec-shrink-image-manifest-tests
  COMMAND ytec-shrink-image-manifest-tests
)

add_executable(
  ytec-migration-engine-tests
  Unit/migration_engine_tests.cpp
)

target_link_libraries(
  ytec-migration-engine-tests
  PRIVATE
    ytec::migration_engine
)

ytec_configure_target(ytec-migration-engine-tests)
add_test(
  NAME ytec-migration-engine-tests
  COMMAND ytec-migration-engine-tests
)

add_executable(
  ytec-partition-snapshot-capture-tests
  Unit/partition_snapshot_capture_tests.cpp
)

target_link_libraries(
  ytec-partition-snapshot-capture-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-partition-snapshot-capture-tests)
add_test(
  NAME ytec-partition-snapshot-capture-tests
  COMMAND ytec-partition-snapshot-capture-tests
)

add_executable(
  ytec-job-manifest-tests
  Unit/job_manifest_tests.cpp
)

target_link_libraries(
  ytec-job-manifest-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-job-manifest-tests)
add_test(NAME ytec-job-manifest-tests COMMAND ytec-job-manifest-tests)

add_executable(
  ytec-dcimg-stream-tests
  Unit/dcimg_stream_tests.cpp
)

target_link_libraries(
  ytec-dcimg-stream-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-dcimg-stream-tests)
add_test(NAME ytec-dcimg-stream-tests COMMAND ytec-dcimg-stream-tests)

add_executable(
  ytec-windows-file-staging-tests
  Unit/windows_file_staging_tests.cpp
)

target_link_libraries(
  ytec-windows-file-staging-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-windows-file-staging-tests)
add_test(
  NAME ytec-windows-file-staging-tests
  COMMAND ytec-windows-file-staging-tests
)

add_executable(
  ytec-restore-image-read-session-tests
  Unit/restore_image_read_session_tests.cpp
)

target_link_libraries(
  ytec-restore-image-read-session-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-restore-image-read-session-tests)
add_test(
  NAME ytec-restore-image-read-session-tests
  COMMAND ytec-restore-image-read-session-tests
)

add_executable(
  ytec-vss-snapshot-image-tests
  Unit/vss_snapshot_image_tests.cpp
)

target_link_libraries(
  ytec-vss-snapshot-image-tests
  PRIVATE
    ytec::vss_requester
)

ytec_configure_target(ytec-vss-snapshot-image-tests)
add_test(
  NAME ytec-vss-snapshot-image-tests
  COMMAND ytec-vss-snapshot-image-tests
)

add_executable(
  ytec-vss-snapshot-plan-tests
  Unit/vss_snapshot_plan_tests.cpp
)

target_link_libraries(
  ytec-vss-snapshot-plan-tests
  PRIVATE
    ytec::vss_requester
)

ytec_configure_target(ytec-vss-snapshot-plan-tests)
add_test(
  NAME ytec-vss-snapshot-plan-tests
  COMMAND ytec-vss-snapshot-plan-tests
)

add_executable(
  ytec-online-backup-tests
  Unit/online_backup_tests.cpp
)

target_link_libraries(
  ytec-online-backup-tests
  PRIVATE
    ytec::vss_requester
)

ytec_configure_target(ytec-online-backup-tests)
add_test(
  NAME ytec-online-backup-tests
  COMMAND ytec-online-backup-tests
)

add_executable(
  ytec-phase2-synthetic-restore-tests
  Integration/synthetic_image_restore_tests.cpp
)

target_link_libraries(
  ytec-phase2-synthetic-restore-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-phase2-synthetic-restore-tests)
add_test(
  NAME ytec-phase2-synthetic-restore-tests
  COMMAND ytec-phase2-synthetic-restore-tests
)

add_executable(
  ytec-mbr-tests
  Unit/mbr_tests.cpp
)

target_link_libraries(
  ytec-mbr-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-mbr-tests)
add_test(NAME ytec-mbr-tests COMMAND ytec-mbr-tests)

add_executable(
  ytec-phase3-synthetic-tests
  Integration/synthetic_mbr_clone_tests.cpp
)

target_link_libraries(
  ytec-phase3-synthetic-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-phase3-synthetic-tests)
add_test(NAME ytec-phase3-synthetic-tests COMMAND ytec-phase3-synthetic-tests)

if(YTEC_BUILD_DESTRUCTIVE_VM_TESTS)
  add_library(
    ytec-vm-clone-execution
    STATIC
      VM/vm_clone_execution_service.cpp
  )

  target_include_directories(
    ytec-vm-clone-execution
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/VM
  )

  target_link_libraries(
    ytec-vm-clone-execution
    PUBLIC
      ytec::disk_model
      ytec::winpe_app_logic
  )

  target_compile_definitions(
    ytec-vm-clone-execution
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-vm-clone-execution)

  add_executable(
    ytec-phase1-physical-clone-vm
    VM/physical_clone_vm.cpp
  )

  target_link_libraries(
    ytec-phase1-physical-clone-vm
    PRIVATE
      ytec-vm-clone-execution
  )

  target_compile_definitions(
    ytec-phase1-physical-clone-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase1-physical-clone-vm)

  add_executable(
    ytec-winpe-clone-vm
    VM/winpe_clone_vm.cpp
  )

  target_link_libraries(
    ytec-winpe-clone-vm
    PRIVATE
      ytec-vm-clone-execution
  )

  target_compile_definitions(
    ytec-winpe-clone-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-winpe-clone-vm)

  add_executable(
    ytec-product-job-fixture-vm
    VM/product_job_fixture_vm.cpp
  )

  target_link_libraries(
    ytec-product-job-fixture-vm
    PRIVATE
      ytec-vm-clone-execution
      ytec::windows_app
  )

  target_compile_definitions(
    ytec-product-job-fixture-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-product-job-fixture-vm)

  add_executable(
    ytec-product-mbr2gpt-verifier-vm
    VM/product_mbr2gpt_verifier_vm.cpp
  )

  target_link_libraries(
    ytec-product-mbr2gpt-verifier-vm
    PRIVATE
      ytec-vm-clone-execution
  )

  target_compile_definitions(
    ytec-product-mbr2gpt-verifier-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-product-mbr2gpt-verifier-vm)

  add_executable(
    ytec-product-job-cancellation-vm
    VM/product_job_cancellation_vm.cpp
  )

  target_link_libraries(
    ytec-product-job-cancellation-vm
    PRIVATE
      ytec-vm-clone-execution
  )

  target_compile_definitions(
    ytec-product-job-cancellation-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-product-job-cancellation-vm)

  add_executable(
    ytec-product-auto-once-monitor-vm
    VM/product_auto_once_monitor_vm.cpp
  )

  target_link_libraries(
    ytec-product-auto-once-monitor-vm
    PRIVATE
      ytec-vm-clone-execution
  )

  target_compile_definitions(
    ytec-product-auto-once-monitor-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-product-auto-once-monitor-vm)

  add_executable(
    ytec-product-job-failure-vm
    VM/product_job_failure_vm.cpp
  )

  target_link_libraries(
    ytec-product-job-failure-vm
    PRIVATE
      ytec-vm-clone-execution
  )

  target_compile_definitions(
    ytec-product-job-failure-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-product-job-failure-vm)

  add_executable(
    ytec-phase1-bcdboot-vm
    VM/boot_repair_vm.cpp
  )

  target_link_libraries(
    ytec-phase1-bcdboot-vm
    PRIVATE
      ytec::boot_repair
      ytec::disk_model
  )

  target_compile_definitions(
    ytec-phase1-bcdboot-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase1-bcdboot-vm)

  add_executable(
    ytec-phase3-bcdboot-vm
    VM/mbr_boot_repair_vm.cpp
  )

  target_link_libraries(
    ytec-phase3-bcdboot-vm
    PRIVATE
      ytec-vm-clone-execution
      ytec::boot_repair
  )

  target_compile_definitions(
    ytec-phase3-bcdboot-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase3-bcdboot-vm)

  add_executable(
    ytec-phase4-mbr2gpt-vm
    VM/mbr2gpt_vm.cpp
  )

  target_link_libraries(
    ytec-phase4-mbr2gpt-vm
    PRIVATE
      ytec-vm-clone-execution
      ytec::boot_repair
  )

  target_compile_definitions(
    ytec-phase4-mbr2gpt-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase4-mbr2gpt-vm)

  add_executable(
    ytec-phase4-bcdboot-vm
    VM/boot_repair_vm.cpp
  )

  target_link_libraries(
    ytec-phase4-bcdboot-vm
    PRIVATE
      ytec::boot_repair
      ytec::disk_model
  )

  target_compile_definitions(
    ytec-phase4-bcdboot-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
      YTEC_VM_PHASE4_BCDBOOT=1
      YTEC_VM_BCDBOOT_TARGET_GIB=56
  )

  ytec_configure_target(ytec-phase4-bcdboot-vm)

  add_executable(
    ytec-phase5-vss-live-vm
    VM/vss_live_validation.cpp
  )

  target_link_libraries(
    ytec-phase5-vss-live-vm
    PRIVATE
      ytec::vss_requester
  )

  target_compile_definitions(
    ytec-phase5-vss-live-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase5-vss-live-vm)
  if(MSVC AND YTEC_ENABLE_MSVC_ANALYZE)
    # Windows SDK 10.0.26100のRegOpenKeyExW SAL注釈自体がC6553を出す。
    # 抑制はVirtualBox判定を持つVM専用ハーネス1ターゲットだけに限定する。
    target_compile_options(ytec-phase5-vss-live-vm PRIVATE /wd6553)
  endif()

  add_executable(
    ytec-phase5-file-staging-vm
    VM/file_staging_validation.cpp
  )

  target_link_libraries(
    ytec-phase5-file-staging-vm
    PRIVATE
      advapi32
      ytec::disk_model
      ytec::image_format
  )

  target_compile_definitions(
    ytec-phase5-file-staging-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase5-file-staging-vm)
  if(MSVC AND YTEC_ENABLE_MSVC_ANALYZE)
    # Windows SDK 10.0.26100のRegOpenKeyExW SAL注釈自体がC6553を出す。
    # 抑制はVirtualBox判定を持つVM専用ハーネス1ターゲットだけに限定する。
    target_compile_options(ytec-phase5-file-staging-vm PRIVATE /wd6553)
  endif()

  add_executable(
    ytec-product-vss-backup-vm
    VM/product_vss_backup_vm.cpp
  )

  target_link_libraries(
    ytec-product-vss-backup-vm
    PRIVATE
      advapi32
      ytec::disk_model
      ytec::windows_app
  )

  target_compile_definitions(
    ytec-product-vss-backup-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-product-vss-backup-vm)

  add_executable(
    ytec-product-data-shrink-vm
    VM/product_data_shrink_vm.cpp
  )

  target_link_libraries(
    ytec-product-data-shrink-vm
    PRIVATE
      advapi32
      ytec::disk_model
      ytec::windows_app
      ytec::winpe_app_logic
  )

  target_compile_definitions(
    ytec-product-data-shrink-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-product-data-shrink-vm)

  add_executable(
    ytec-product-vss-restore-vm
    VM/product_vss_restore_vm.cpp
  )

  target_link_libraries(
    ytec-product-vss-restore-vm
    PRIVATE
      ytec-vm-clone-execution
      ytec::windows_app
  )

  target_compile_definitions(
    ytec-product-vss-restore-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-product-vss-restore-vm)
endif()
