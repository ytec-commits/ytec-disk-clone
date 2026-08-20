#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi.h"
#include "ytec/imageformat/tsumugi_manifest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kRandomSeed = 0x5954454346555A5AULL;
constexpr std::size_t kMutationIterations = 4096U;
constexpr std::size_t kMaximumInputBytes = 64U * 1024U;
constexpr std::uint64_t kDiskBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionOffset = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionBytes = 8ULL * 1024ULL * 1024ULL;

class DeterministicRandom final {
 public:
  explicit DeterministicRandom(const std::uint64_t seed) noexcept
      : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ ^= state_ >> 12U;
    state_ ^= state_ << 25U;
    state_ ^= state_ >> 27U;
    return state_ * 0x2545F4914F6CDD1DULL;
  }

  [[nodiscard]] std::size_t index(const std::size_t upper_bound) noexcept {
    if (upper_bound == 0U) {
      return 0U;
    }
    return static_cast<std::size_t>(next() % upper_bound);
  }

  [[nodiscard]] std::byte byte() noexcept {
    return static_cast<std::byte>(next() & 0xFFU);
  }

 private:
  std::uint64_t state_{};
};

struct FuzzStatistics final {
  std::size_t inputs{};
  std::size_t accepted_tsumugi{};
  std::size_t accepted_manifests{};
  std::size_t accepted_snapshots{};
};

std::vector<std::byte> make_partition_snapshot_seed() {
  using namespace ytec::imageformat;
  PartitionSnapshot snapshot{
      .style = PartitionTableStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
  };
  PartitionTableRegion region;
  region.disk_offset = 0U;
  region.data.assign(512U, std::byte{0});
  region.data[446U + 4U] = std::byte{0x07};
  region.data[510U] = std::byte{0x55};
  region.data[511U] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(region));

  auto encoded = build_partition_snapshot_v1(snapshot);
  if (!encoded.has_value()) {
    throw std::runtime_error("failed to build partition snapshot seed");
  }
  return encoded.take_value();
}

std::vector<std::byte> make_manifest_seed(
    const std::vector<std::byte>& partition_snapshot) {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::exact,
      .partition_style = TsumugiManifestPartitionStyle::mbr,
      .flags = TsumugiManifestFlags::source_contains_windows |
          TsumugiManifestFlags::automatic_surplus_allocation,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-10T00:00:00Z",
      .app_version = "image-fuzz-seed",
      .partition_snapshot = partition_snapshot,
  };
  manifest.source_model_hash[0] = std::byte{0x11};
  manifest.source_serial_hash[0] = std::byte{0x22};
  manifest.source_state_hash[0] = std::byte{0x33};

  TsumugiManifestPartition partition{
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = TsumugiManifestPartitionRole::windows,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::selected |
          TsumugiManifestPartitionFlags::required |
          TsumugiManifestPartitionFlags::active |
          TsumugiManifestPartitionFlags::contains_windows,
      .source_offset = kPartitionOffset,
      .source_size = kPartitionBytes,
      .used_bytes = 3ULL * 1024ULL * 1024ULL,
      .minimum_target_bytes = kPartitionBytes,
      .planned_target_bytes = kPartitionBytes,
      .payload_logical_offset = kPartitionOffset,
      .payload_logical_length = kPartitionBytes,
      .name_utf8 = "Windows",
      .label_utf8 = "System",
  };
  partition.type_id[0] = std::byte{0x07};
  manifest.partitions.push_back(std::move(partition));

  auto encoded = build_tsumugi_manifest_v1(manifest);
  if (!encoded.has_value()) {
    throw std::runtime_error("failed to build Tsumugi manifest seed");
  }
  return encoded.take_value();
}

std::vector<std::byte> make_tsumugi_seed(
    const std::vector<std::byte>& manifest) {
  using namespace ytec::imageformat;
  TsumugiBuildRequest request{
      .payload_kind = TsumugiPayloadKind::exact_disk,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .chunk_size = kImageChunkSize16MiB,
      .compression = ImageCompression::none,
      .manifest = manifest,
  };
  request.image_id[0] = std::byte{0x59};
  request.image_id[1] = std::byte{0x54};
  request.image_id[2] = std::byte{0x45};
  request.image_id[3] = std::byte{0x43};

  TsumugiBuildChunk zero_chunk{
      .logical_offset = 0U,
      .logical_length = 512U,
      .flags = TsumugiChunkFlags::zero_filled,
  };
  request.chunks.push_back(std::move(zero_chunk));

  auto encoded = build_tsumugi_v1(request);
  if (!encoded.has_value()) {
    throw std::runtime_error("failed to build .tsumugi seed");
  }
  return encoded.take_value();
}

std::vector<std::vector<std::byte>> make_seed_corpus() {
  auto snapshot = make_partition_snapshot_seed();
  auto manifest = make_manifest_seed(snapshot);
  auto tsumugi = make_tsumugi_seed(manifest);

  std::vector<std::vector<std::byte>> corpus;
  corpus.emplace_back();
  corpus.emplace_back(1U, std::byte{0});
  corpus.emplace_back(511U, std::byte{0});
  corpus.emplace_back(512U, std::byte{0xFF});
  corpus.emplace_back(kMaximumInputBytes, std::byte{0});
  corpus.push_back(std::move(snapshot));
  corpus.push_back(std::move(manifest));
  corpus.push_back(std::move(tsumugi));
  return corpus;
}

void fill_random(
    std::vector<std::byte>& bytes,
    DeterministicRandom& random) noexcept {
  for (auto& value : bytes) {
    value = random.byte();
  }
}

std::vector<std::byte> mutate(
    const std::vector<std::vector<std::byte>>& corpus,
    DeterministicRandom& random) {
  std::vector<std::byte> input = corpus[random.index(corpus.size())];
  const std::size_t operation = random.index(7U);

  if (operation == 0U) {
    input.assign(random.index(kMaximumInputBytes + 1U), std::byte{0});
    fill_random(input, random);
  } else if (operation == 1U && !input.empty()) {
    const std::size_t changes = 1U + random.index(16U);
    for (std::size_t index = 0; index < changes; ++index) {
      input[random.index(input.size())] ^= random.byte();
    }
  } else if (operation == 2U && !input.empty()) {
    input.resize(random.index(input.size()));
  } else if (operation == 3U && input.size() < kMaximumInputBytes) {
    const std::size_t available = kMaximumInputBytes - input.size();
    const std::size_t appended = 1U + random.index(
        (std::min)(available, static_cast<std::size_t>(256U)));
    const std::size_t old_size = input.size();
    input.resize(old_size + appended);
    for (std::size_t index = old_size; index < input.size(); ++index) {
      input[index] = random.byte();
    }
  } else if (operation == 4U && !input.empty()) {
    const std::size_t first = random.index(input.size());
    const std::size_t available = input.size() - first;
    const std::size_t count = 1U + random.index(available);
    input.erase(
        input.begin() + static_cast<std::ptrdiff_t>(first),
        input.begin() + static_cast<std::ptrdiff_t>(first + count));
  } else if (operation == 5U && !input.empty()) {
    const std::size_t first = random.index(input.size());
    const std::size_t count = (std::min)(
        input.size() - first,
        1U + random.index(32U));
    const std::byte replacement =
        random.index(2U) == 0U ? std::byte{0} : std::byte{0xFF};
    std::fill_n(
        input.begin() + static_cast<std::ptrdiff_t>(first),
        count,
        replacement);
  } else if (operation == 6U && input.size() < kMaximumInputBytes) {
    const std::size_t insert_at = random.index(input.size() + 1U);
    const std::size_t count = 1U + random.index(
        (std::min)(
            kMaximumInputBytes - input.size(),
            static_cast<std::size_t>(64U)));
    input.insert(
        input.begin() + static_cast<std::ptrdiff_t>(insert_at),
        count,
        random.byte());
  }

  if (input.size() > kMaximumInputBytes) {
    throw std::runtime_error("mutator exceeded the fixed input bound");
  }
  return input;
}

void exercise_input(
    const std::vector<std::byte>& input,
    FuzzStatistics& statistics) {
  using namespace ytec::imageformat;
  ++statistics.inputs;

  const auto tsumugi = inspect_tsumugi_v1(input);
  if (tsumugi.has_value()) {
    ++statistics.accepted_tsumugi;
    const auto& inspected = tsumugi.value();
    if (!inspected.header_hash_verified ||
        !inspected.all_chunks_verified ||
        !inspected.global_hash_verified) {
      throw std::runtime_error(
          "accepted .tsumugi lacks a required verification result");
    }
  }

  const auto manifest = inspect_tsumugi_manifest_v1(input);
  if (manifest.has_value()) {
    ++statistics.accepted_manifests;
    const auto rebuilt = build_tsumugi_manifest_v1(manifest.value());
    if (!rebuilt.has_value() || rebuilt.value() != input) {
      throw std::runtime_error(
          "accepted Tsumugi manifest is not canonical");
    }
  }

  const auto snapshot = inspect_partition_snapshot_v1(input);
  if (snapshot.has_value()) {
    ++statistics.accepted_snapshots;
    const auto rebuilt = build_partition_snapshot_v1(snapshot.value());
    if (!rebuilt.has_value() || rebuilt.value() != input) {
      throw std::runtime_error(
          "accepted partition snapshot is not canonical");
    }
  }
}

}  // namespace

int main() {
  std::size_t current_case = 0U;
  try {
    auto corpus = make_seed_corpus();
    FuzzStatistics statistics;
    for (const auto& seed : corpus) {
      exercise_input(seed, statistics);
      ++current_case;
    }

    DeterministicRandom random(kRandomSeed);
    for (std::size_t iteration = 0U;
         iteration < kMutationIterations;
         ++iteration) {
      const auto input = mutate(corpus, random);
      exercise_input(input, statistics);
      ++current_case;
    }

    std::cout
        << "PASS ytec-image-fuzz-tests seed=0x5954454346555A5A"
        << " cases=" << statistics.inputs
        << " max_input_bytes=" << kMaximumInputBytes
        << " accepted_tsumugi=" << statistics.accepted_tsumugi
        << " accepted_manifests=" << statistics.accepted_manifests
        << " accepted_snapshots=" << statistics.accepted_snapshots
        << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr
        << "FAIL ytec-image-fuzz-tests seed=0x5954454346555A5A"
        << " case=" << current_case
        << ": " << exception.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr
        << "FAIL ytec-image-fuzz-tests seed=0x5954454346555A5A"
        << " case=" << current_case
        << ": unknown exception\n";
    return 1;
  }
}
