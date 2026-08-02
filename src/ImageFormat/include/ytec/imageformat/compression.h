#pragma once

#include "ytec/clonecore/result.h"

#include <cstddef>
#include <span>
#include <vector>

namespace ytec::imageformat {

inline constexpr int kDcimgZstandardCompressionLevel = 3;

// dcimg v1 Zstandard profile: one standard frame, no dictionary, exact content
// size, at most 32 MiB uncompressed. The outer dcimg SHA-256 remains mandatory.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
compress_zstandard_dcimg_v1(std::span<const std::byte> uncompressed);

// Rejects unknown content sizes, concatenated/trailing frames, dictionaries,
// expansion beyond the exact declared length, and all Zstandard errors.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
decompress_zstandard_dcimg_v1(
    std::span<const std::byte> stored,
    std::size_t expected_uncompressed_length);

}  // namespace ytec::imageformat
