#include "ytec/diskmodel/physical_disk.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::diskmodel {
namespace {

using clonecore::Error;
using clonecore::ErrorCode;
using clonecore::Result;
using clonecore::UniqueHandle;
using clonecore::VolumeBitmapBinding;

constexpr std::size_t kVolumeNameCharacters = 32U * 1024U;
constexpr std::size_t kExtentBufferBytes = 64U * 1024U;
constexpr std::size_t kMaximumExtentCount = 256;

struct ExpectedVolumePartition final {
  std::uint32_t table_index{};
  std::uint64_t offset_bytes{};
};

Error mapping_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

class VolumeSearch final {
 public:
  explicit VolumeSearch(const HANDLE handle) noexcept : handle_(handle) {}
  ~VolumeSearch() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      FindVolumeClose(handle_);
    }
  }

  VolumeSearch(const VolumeSearch&) = delete;
  VolumeSearch& operator=(const VolumeSearch&) = delete;

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

Result<VOLUME_DISK_EXTENTS> query_single_extent(const HANDLE volume) {
  std::vector<std::byte> buffer(kExtentBufferBytes);
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          volume,
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &bytes_returned,
          nullptr)) {
    return Result<VOLUME_DISK_EXTENTS>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"ボリュームの物理ディスク対応取得",
        GetLastError()));
  }
  constexpr std::size_t header_size =
      offsetof(VOLUME_DISK_EXTENTS, Extents);
  if (bytes_returned < header_size) {
    return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ボリュームの物理ディスク対応検証",
        L"ディスク範囲応答が短すぎます"));
  }
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (extents->NumberOfDiskExtents == 0 ||
      extents->NumberOfDiskExtents > kMaximumExtentCount) {
    return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ボリュームの物理ディスク対応検証",
        L"ディスク範囲件数が不正です"));
  }
  const std::size_t required = header_size +
      static_cast<std::size_t>(extents->NumberOfDiskExtents) *
          sizeof(DISK_EXTENT);
  if (required > bytes_returned) {
    return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ボリュームの物理ディスク対応検証",
        L"ディスク範囲応答に全要素がありません"));
  }
  if (extents->NumberOfDiskExtents != 1) {
    return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"ボリュームの物理ディスク対応検証",
        L"複数ディスクへまたがるボリュームは対応しません"));
  }
  VOLUME_DISK_EXTENTS single{};
  single.NumberOfDiskExtents = 1;
  single.Extents[0] = extents->Extents[0];
  return Result<VOLUME_DISK_EXTENTS>::success(single);
}

Result<std::vector<VolumeBitmapBinding>> query_bindings(
    const DiskInfo& source_disk,
    const std::span<const ExpectedVolumePartition> expected_partitions) {
  std::vector<wchar_t> volume_name(kVolumeNameCharacters, L'\0');
  const HANDLE search_handle = FindFirstVolumeW(
      volume_name.data(), static_cast<DWORD>(volume_name.size()));
  if (search_handle == INVALID_HANDLE_VALUE) {
    return Result<std::vector<VolumeBitmapBinding>>::failure(
        clonecore::make_win32_error(
            ErrorCode::enumeration_failed,
            L"Windowsボリューム列挙の開始",
            GetLastError()));
  }
  VolumeSearch search(search_handle);
  std::vector<VolumeBitmapBinding> bindings;
  for (;;) {
    const std::wstring safe_volume_name(volume_name.data());
    std::wstring open_path = safe_volume_name;
    if (!open_path.empty() && open_path.back() == L'\\') {
      open_path.pop_back();
    }
    UniqueHandle volume(CreateFileW(
        open_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (volume) {
      const auto extent = query_single_extent(volume.get());
      if (extent &&
          extent.value().Extents[0].DiskNumber == source_disk.disk_number &&
          extent.value().Extents[0].StartingOffset.QuadPart >= 0) {
        const std::uint64_t start = static_cast<std::uint64_t>(
            extent.value().Extents[0].StartingOffset.QuadPart);
        const auto partition = std::find_if(
            expected_partitions.begin(),
            expected_partitions.end(),
            [&](const auto& candidate) {
              return candidate.offset_bytes == start;
            });
        if (partition != expected_partitions.end()) {
          const bool duplicate = std::any_of(
              bindings.begin(), bindings.end(), [&](const auto& binding) {
                return binding.partition_entry_index ==
                       partition->table_index;
              });
          if (duplicate) {
            return Result<std::vector<VolumeBitmapBinding>>::failure(
                mapping_error(
                    ErrorCode::identity_mismatch,
                    ERROR_DUP_NAME,
                    L"NTFSボリューム対応付け",
                    L"同じパーティションへ複数のVolume GUIDが対応しました"));
          }
          bindings.push_back(VolumeBitmapBinding{
              .partition_entry_index = partition->table_index,
              .volume_device_path = safe_volume_name,
          });
        }
      }
    }

    std::fill(volume_name.begin(), volume_name.end(), L'\0');
    if (!FindNextVolumeW(
            search_handle,
            volume_name.data(),
            static_cast<DWORD>(volume_name.size()))) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_NO_MORE_FILES) {
        break;
      }
      return Result<std::vector<VolumeBitmapBinding>>::failure(
          clonecore::make_win32_error(
              ErrorCode::enumeration_failed,
              L"Windowsボリューム列挙の反復",
              native_code));
    }
  }

  for (const auto& partition : expected_partitions) {
    const bool found = std::any_of(
        bindings.begin(), bindings.end(), [&](const auto& binding) {
          return binding.partition_entry_index == partition.table_index;
        });
    if (!found) {
      return Result<std::vector<VolumeBitmapBinding>>::failure(mapping_error(
          ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"NTFSボリューム対応付け",
        L"対象NTFSパーティションのVolume GUIDを一意に特定できません"));
    }
  }
  return Result<std::vector<VolumeBitmapBinding>>::success(
      std::move(bindings));
}

Result<std::uint64_t> checked_partition_offset(
    const std::uint64_t first_lba,
    const std::uint32_t sector_size,
    const std::wstring_view operation) {
  if (sector_size == 0 ||
      first_lba > std::numeric_limits<std::uint64_t>::max() / sector_size) {
    return Result<std::uint64_t>::failure(mapping_error(
        ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"パーティション開始位置がオーバーフローしました"));
  }
  return Result<std::uint64_t>::success(first_lba * sector_size);
}

}  // namespace

Result<std::vector<VolumeBitmapBinding>>
query_windows_volume_bitmap_bindings(
    const DiskInfo& source_disk,
    const clonecore::GptDisk& source_gpt) {
  if (source_gpt.logical_sector_size == 0 ||
      source_gpt.sector_count >
          std::numeric_limits<std::uint64_t>::max() /
              source_gpt.logical_sector_size ||
      source_disk.disk_number == std::numeric_limits<std::uint32_t>::max() ||
      source_disk.size_bytes != source_gpt.sector_count *
                                      source_gpt.logical_sector_size ||
      source_disk.logical_sector_size != source_gpt.logical_sector_size) {
    return Result<std::vector<VolumeBitmapBinding>>::failure(mapping_error(
        ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"ボリューム対応付け対象の再確認",
        L"再列挙ディスクとGPT解析結果の寸法が一致しません"));
  }

  std::vector<ExpectedVolumePartition> expected;
  for (const auto& partition : source_gpt.partitions) {
    if (partition.type_guid != clonecore::gpt_type_basic_data()) {
      continue;
    }
    const auto offset = checked_partition_offset(
        partition.first_lba,
        source_gpt.logical_sector_size,
        L"GPTパーティション開始位置計算");
    if (!offset) {
      return Result<std::vector<VolumeBitmapBinding>>::failure(offset.error());
    }
    expected.push_back(ExpectedVolumePartition{
        .table_index = partition.entry_index,
        .offset_bytes = offset.value(),
    });
  }
  return query_bindings(source_disk, expected);
}

Result<std::vector<VolumeBitmapBinding>>
query_windows_volume_bitmap_bindings(
    const DiskInfo& source_disk,
    const clonecore::MbrDisk& source_mbr) {
  if (source_mbr.logical_sector_size == 0 ||
      source_mbr.sector_count >
          std::numeric_limits<std::uint64_t>::max() /
              source_mbr.logical_sector_size ||
      source_disk.disk_number == std::numeric_limits<std::uint32_t>::max() ||
      source_disk.size_bytes != source_mbr.sector_count *
                                      source_mbr.logical_sector_size ||
      source_disk.logical_sector_size != source_mbr.logical_sector_size) {
    return Result<std::vector<VolumeBitmapBinding>>::failure(mapping_error(
        ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"ボリューム対応付け対象の再確認",
        L"再列挙ディスクとMBR解析結果の寸法が一致しません"));
  }

  std::vector<ExpectedVolumePartition> expected;
  for (const auto& partition : source_mbr.partitions) {
    if (partition.type != 0x07) {
      continue;
    }
    const auto offset = checked_partition_offset(
        partition.first_lba,
        source_mbr.logical_sector_size,
        L"MBRパーティション開始位置計算");
    if (!offset) {
      return Result<std::vector<VolumeBitmapBinding>>::failure(offset.error());
    }
    expected.push_back(ExpectedVolumePartition{
        .table_index = partition.table_index,
        .offset_bytes = offset.value(),
    });
  }
  return query_bindings(source_disk, expected);
}

}  // namespace ytec::diskmodel
