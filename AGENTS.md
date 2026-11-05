# Repository Guidelines

## Project Structure & Module Organization

`src/` contains the C++20 engine and CLI: corpus preparation, BM25 indexing, TCP shards, the coordinator, and load generation. `tests/` holds GoogleTest cases and CLI integration scripts. `scripts/` generates benchmark data and plots; `bench/` contains the published chart and summary. `docs/` contains the project plan and learning notes.

## Build, Test & Development Commands

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

These commands configure, build, and run the full test suite. The first test configure downloads GoogleTest. For a local search example, follow [README.md](README.md#try-it). Run `scripts/bench.sh baseline` to reproduce a benchmark sweep; results go to `bench/results/`.

## Coding Style & Naming Conventions

Use two-space indentation and the Google-based `.clang-format` style with a 100-column limit. Keep filenames and functions in `snake_case` and types in `PascalCase`, following nearby code. Check C++ formatting with:

```bash
clang-format-18 --dry-run --Werror src/*.cpp src/*.hpp tests/*.cpp
```

Use `clang-tidy-18` with a compilation database as described in [CONTRIBUTING.md](CONTRIBUTING.md). Avoid new dependencies for functionality already covered by C++20 or POSIX.

## Testing Guidelines

Add focused `*_test.cpp` cases in `tests/` for logic changes. Use the existing `*_cli_test.sh` pattern for process and socket behavior. Run CTest before opening a PR; CI also runs GCC, Clang, ASan/UBSan, and TSan builds. There is no numeric coverage threshold.

## Commits & Pull Requests

Use short, descriptive subjects consistent with history, such as `feat: add deadlines`, `feat(coordinator): add failover`, or `chore(docs): clarify benchmarks`. In a PR, summarize the behavior change, list tests run, and link a relevant issue when one exists. Include screenshots only for visual changes.

## Architecture & Security

Every shard must load `global_stats.bin` from the same preparation run as its partition; mixing runs makes scores incomparable. Shard TCP endpoints have no authentication or encryption. Bind to loopback or use a trusted network.
