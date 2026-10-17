// Command-line entry point and role selection placeholder
#include <cstdio>
#include <string_view>

namespace {

int usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s <role> [args]\n"
               "roles:\n"
               "  prepare      build global_stats.bin from a corpus\n"
               "  shard        serve one corpus partition\n"
               "  coordinator  fan queries out to shards\n",
               argv0);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage(argv[0]);
  const std::string_view role = argv[1];

  if (role == "prepare") {
    std::fprintf(stderr, "prepare: not implemented yet (plan day 1)\n");
  } else if (role == "shard") {
    std::fprintf(stderr, "shard: not implemented yet (plan day 2)\n");
  } else if (role == "coordinator") {
    std::fprintf(stderr, "coordinator: not implemented yet (plan day 3)\n");
  } else {
    return usage(argv[0]);
  }
  return 1;
}
