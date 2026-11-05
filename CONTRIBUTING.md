# Contributing to KRetrieve

Build and test instructions are in the [README](README.md#try-it).

## Formatting and static analysis

CI uses LLVM 18 for consistent formatting and static-analysis results. On Ubuntu
24.04, install the tools with:

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends -y clang-18 clang-format-18 clang-tidy-18
```

Check formatting, or replace `--dry-run --Werror` with `-i` to format the files:

```bash
clang-format-18 --dry-run --Werror src/*.cpp src/*.hpp tests/*.cpp
```

Run the focused correctness checks in `.clang-tidy` using the real compilation
settings. The build prepares dependencies; only project sources are analyzed,
and diagnostics from downloaded GoogleTest headers are excluded.

```bash
cmake -S . -B build-quality \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_TESTING=ON
cmake --build build-quality --parallel 2
clang-tidy-18 -p build-quality src/*.cpp tests/*.cpp
```

Selected static-analysis findings fail the check. Compiler warnings remain
enabled with `-Wall -Wextra -Wpedantic`; static analysis does not replace tests
or sanitizers.

## CI and releases

`.github/workflows/ci.yml` runs on pull requests, pushes to `main`, version-tag
pushes, and manual dispatch. Its required checks are:

| Check | Configuration |
|---|---|
| `quality` | LLVM 18 formatting and focused static analysis |
| `gcc-release` | GCC 13, Release build, all CTest tests |
| `clang-release` | Clang 18, Release build, all CTest tests |
| `clang-sanitizers` | Clang 18, Debug build, ASan + UBSan, all CTest tests |
| `clang-tsan` | Clang 18, RelWithDebInfo build, TSan, all CTest tests |

All jobs run on Ubuntu 24.04. Tests have a 60-second per-test timeout, and failed
build/test jobs upload diagnostics for seven days. PR runs need no secrets and
cannot publish releases. Superseded PR runs are canceled; release runs are not.

### Publish a version

After merging and validating the intended commit on `main`, a maintainer can
publish an annotated tag:

```bash
git switch main
git pull --ff-only
git tag -a v0.1.0 -m "KRetrieve v0.1.0"
git push origin v0.1.0
```

Accepted versions are `vMAJOR.MINOR.PATCH` and `vMAJOR.MINOR.PATCH-rc.N`, with
no leading zeroes and a positive release-candidate number. Use a clearly labeled
release candidate, such as `v0.1.0-rc.1`, for the first end-to-end publication
test. Manual workflow dispatch validates code but never publishes a release.

Publishing requires all five checks to pass for the tagged commit and verifies
that the commit belongs to `main` history. The release job packages the **same
GCC Release executable that passed the tests**, verifies its checksum, extracts
it, and runs the shard and coordinator CLI integration tests against that extracted
executable.
No rebuild occurs during publishing. Only the release job receives write access,
using GitHub's built-in token; no personal publishing token is needed.

A release stays a draft until both assets are uploaded. Release candidates are
marked as prereleases. Existing releases and their assets are never overwritten.
If publishing fails after draft creation, inspect and explicitly remove only the
incomplete draft before rerunning the workflow. Never move an already published
tag; publish a new version for corrections.

### Repository protection setup (maintainer)

After this workflow is pushed and its five checks have run successfully once,
configure **Settings → Rules → Rulesets**:

- Protect `main`: require pull requests and the five check names above, and block
  force pushes and deletion. Select GitHub Actions as the expected check source.
- Restrict creation of `v*` tags to maintainers/admins using a tag-creation
  ruleset with the appropriate bypass roles.
- Use a separate `v*` tag ruleset to block updates and deletion, without a
  maintainer bypass. Recovery should require an explicit administrator action.

These are repository-side settings, not settings activated by committing YAML.
Before treating the pipeline as fully commissioned, use a temporary PR to verify
that formatting errors, a static-analysis violation, a failing test, and a
sanitizer-detectable error each fail the appropriate check and block merging.
Then validate release-candidate delivery and confirm that failing tagged checks
publish nothing. Do not merge the deliberate failures.

Delivery currently means publishing tested CLI binaries, not deploying a
service. Containers, service deployment, coverage gates, additional
platforms, and build caches are deferred until they address an actual need.
