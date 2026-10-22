// Offline corpus partitioning and versioned global-statistics persistence.
#pragma once

#include "corpus.hpp"

#include <cstddef>
#include <filesystem>

namespace kretrieve {

// Little-endian KRST v1 format, documented in README.md. The writer replaces
// its target file; prepare_corpus calls it only inside a newly created directory.
// Invalid statistics throw std::invalid_argument; file/format errors throw
// std::runtime_error (including std::filesystem::filesystem_error).
void write_global_stats(const std::filesystem::path& path, const GlobalStats& stats);
GlobalStats read_global_stats(const std::filesystem::path& path);

// Validate the entire source before creating output. The output directory must
// not exist and its parent must exist. Write every shard, including empty ones,
// then global_stats.bin. On I/O failure, partial output may remain; do not use it.
void prepare_corpus(const std::filesystem::path& input, const std::filesystem::path& output,
                    size_t shard_count);

}  // namespace kretrieve
