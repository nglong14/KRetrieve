// Command-line entry point and role selection.
#include "prepare.hpp"

#include <charconv>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

int usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s <role> [args]\n"
               "roles:\n"
               "  prepare --input <corpus.tsv> --output <new-directory> --shards <N>\n"
               "  shard        serve one corpus partition\n"
               "  coordinator  fan queries out to shards\n",
               argv0);
  return 2;
}

int run_prepare(int argc, char** argv) {
  std::filesystem::path input;
  std::filesystem::path output;
  size_t shards = 0;
  for (int i = 2; i < argc; i += 2) {
    const std::string_view option = argv[i];
    if (i + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(option));
    }
    const std::string_view value = argv[i + 1];
    if (value.empty()) throw std::invalid_argument("empty value for " + std::string(option));
    if (option == "--input" && input.empty()) {
      input = value;
    } else if (option == "--output" && output.empty()) {
      output = value;
    } else if (option == "--shards" && shards == 0) {
      const auto result = std::from_chars(value.data(), value.data() + value.size(), shards);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || shards == 0) {
        throw std::invalid_argument("--shards must be a positive integer");
      }
    } else {
      throw std::invalid_argument("unknown or duplicate option: " + std::string(option));
    }
  }
  if (input.empty() || output.empty() || shards == 0) {
    throw std::invalid_argument("--input, --output, and --shards are required");
  }
  kretrieve::prepare_corpus(input, output, shards);
  std::printf("Prepared %zu shards in %s\n", shards, output.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage(argv[0]);
  const std::string_view role = argv[1];

  if (role == "prepare") {
    try {
      return run_prepare(argc, argv);
    } catch (const std::invalid_argument& error) {
      std::fprintf(stderr, "prepare: %s\n", error.what());
      return usage(argv[0]);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "prepare: %s\n", error.what());
      return 1;
    }
  } else if (role == "shard") {
    std::fprintf(stderr, "shard: not implemented yet (plan day 2)\n");
  } else if (role == "coordinator") {
    std::fprintf(stderr, "coordinator: not implemented yet (plan day 3)\n");
  } else {
    return usage(argv[0]);
  }
  return 1;
}
