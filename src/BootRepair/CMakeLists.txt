add_library(
  BootRepair
  STATIC
    src/bcdboot.cpp
    src/mbr2gpt.cpp
    src/mbr2gpt_execution.cpp
    src/mbr2gpt_layout.cpp
    src/offline_windows.cpp
    src/registry_hive.cpp
    src/standalone_repair.cpp
    src/system_volume_mount.cpp
    src/winre_diagnostic.cpp
)

add_library(ytec::boot_repair ALIAS BootRepair)

target_include_directories(
  BootRepair
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  BootRepair
  PUBLIC
    ytec::clone_core
    ytec::disk_model
  PRIVATE
    advapi32
    crypt32
    wintrust
)

ytec_configure_target(BootRepair)

if(MSVC AND YTEC_ENABLE_MSVC_ANALYZE)
  # Windows SDK 10.0.26100のRegOpenKeyExW宣言に含まれるSAL注釈が
  # /analyzeでC6553になるため、このターゲットだけSDK警告を抑止する。
  target_compile_options(BootRepair PRIVATE /wd6553)
endif()
