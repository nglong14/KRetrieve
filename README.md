# KRetrieve

[![CI](https://github.com/nglong14/KRetrieve/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/nglong14/KRetrieve/actions/workflows/ci.yml)

A C++20 sharded BM25 retrieval engine in progress. Day 1 provides offline corpus
preparation and immutable, in-process search. Day 2 serves one partition over
TCP. The scatter-gather coordinator is not implemented yet.

## Build and test

Requirements: a C++20 compiler, CMake, and a POSIX platform. GoogleTest is fetched
by CMake when tests are enabled, so the first configure needs network access.

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

ASan + UBSan use a separate build directory:

```bash
cmake -S . -B build-asan -DBUILD_TESTING=ON -DKRETRIEVE_ASAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

`-DKRETRIEVE_TSAN=ON` is also available in a separate build; it cannot be combined
with `KRETRIEVE_ASAN`. The shard server is multithreaded, so run the TSan build
locally after touching it; CI does not run TSan yet.

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

All jobs run on Ubuntu 24.04. Tests have a 60-second per-test timeout, and failed
build/test jobs upload diagnostics for seven days. PR runs need no secrets and
cannot publish releases. Superseded PR runs are canceled; release runs are not.

### Download a release

[GitHub Releases](https://github.com/nglong14/KRetrieve/releases) provides the
`kretrieve-vVERSION-linux-x86_64.tar.gz` archive and `SHA256SUMS`. For example,
after downloading both assets for `v0.1.0` into the same directory:

```bash
sha256sum --check SHA256SUMS
tar -xzf kretrieve-v0.1.0-linux-x86_64.tar.gz
./kretrieve-v0.1.0-linux-x86_64/kretrieve prepare \
  --input corpus.tsv --output prepared --shards 3
```

The archive contains the tested CLI executable, this README, and
`BUILD-METADATA.txt` with the version, commit, compiler, build platform, and
linked-library information. The initial supported runtime is **Ubuntu 24.04
x86-64**, with its standard glibc and libstdc++ runtime libraries. The executable
is dynamically linked, not a portable static binary for all Linux distributions.
The C++ library is not packaged as an installed/exported SDK.

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

Publishing requires all four checks to pass for the tagged commit and verifies
that the commit belongs to `main` history. The release job packages the **same
GCC Release executable that passed the tests**, verifies its checksum, extracts
it, and runs the existing CLI integration test against that extracted executable.
No rebuild occurs during publishing. Only the release job receives write access,
using GitHub's built-in token; no personal publishing token is needed.

A release stays a draft until both assets are uploaded. Release candidates are
marked as prereleases. Existing releases and their assets are never overwritten.
If publishing fails after draft creation, inspect and explicitly remove only the
incomplete draft before rerunning the workflow. Never move an already published
tag; publish a new version for corrections.

### Repository protection setup (maintainer)

After this workflow is pushed and its four checks have run successfully once,
configure **Settings → Rules → Rulesets**:

- Protect `main`: require pull requests and the four check names above, and block
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
service. TSan, containers, service deployment, coverage gates, additional
platforms, and build caches are deferred until they address an actual need.

## Prepare a corpus

The source format is one document per line:

```text
<unsigned 64-bit document ID><TAB><document text>
```

IDs must be globally unique. The first tab separates the ID from the text;
additional tabs are part of the text. Empty text, CRLF, and a final record without
a newline are accepted. Empty source corpora, blank records, invalid IDs, and
duplicate IDs are rejected, with line numbers for malformed records.

For a minimal example (the command below writes real tabs):

```bash
printf '10\tcat cat dog\n20\tdog bird\n30\tcat bird bird\n' > corpus.tsv
./build/kretrieve prepare --input corpus.tsv --output prepared --shards 3
```

The shard count must be positive. The output directory **must not already
exist**, and its parent must exist. Preparation validates the full corpus before
creating output and never overwrites an existing directory or file.

```text
prepared/
  global_stats.bin
  shard-0.tsv
  shard-1.tsv
  shard-2.tsv
```

Partition assignment is `document_id % shard_count`. Each document is written
exactly once, preserving its parsed ID and text. Empty partitions are written as
empty files. Input order is preserved within each partition; statistics terms
are serialized in sorted order for reproducible output.

Preparation currently holds the corpus and statistics in memory. It opens one
partition output at a time. I/O errors return nonzero and may leave partial
output; do not use a failed run's directory. Multi-file atomic publication and
crash durability are not provided. The success summary is printed only after
all files have been closed successfully.

## Load a partition and search

The C++ library exposes corpus loading, statistics persistence, and search:

```cpp
#include "index.hpp"
#include "prepare.hpp"

#include <fstream>

auto stats = kretrieve::read_global_stats("prepared/global_stats.bin");
std::ifstream partition("prepared/shard-0.tsv");
auto documents = kretrieve::load_corpus(partition, /*allow_empty=*/true);
const kretrieve::InvertedIndex index(documents, stats);
auto hits = index.search("cat dog", 10);
```

Each shard must load the **same `global_stats.bin` from the same preparation
run as its partition**. Global document count, document frequencies, and average
length make scores directly comparable between shards. Local term frequencies
and document lengths come from each partition. No coordinator re-ranking is
needed. Artifacts do not contain a corpus identity/checksum; callers must not mix
files from different runs.

The index owns its statistics and indexed metadata, and does not retain document
text. Searches use query-local scratch storage and do not mutate the index.

### Ranking contract

- Tokenization lowercases ASCII, splits on non-alphanumeric characters, removes
  the built-in English stopwords, and does not stem.
- Document length counts tokens after stopword removal. Empty-token documents
  still count toward global `N`; an all-empty-token corpus has `avgdl = 0` and
  returns no search hits.
- Queries use the same tokenizer, with distinct terms processed in sorted order.
- Matching is OR-based; unknown terms are ignored and repeated query terms do
  not multiply scores. Empty queries and `K = 0` return no hits.
- Results are ordered by score descending, then document ID ascending for exact
  score ties. Scores and average document length use `double`.

BM25 uses `k1 = 1.2`, `b = 0.75`, and natural logarithms:

```text
IDF(t) = ln(1 + (N - df(t) + 0.5) / (df(t) + 0.5))

score(d, q) = sum over distinct query terms t of:
  IDF(t) * tf(t,d) * (k1 + 1)
  / (tf(t,d) + k1 * (1 - b + b * length(d) / avgdl))
```

For the example corpus, `N = 3`, `avgdl = 8/3`, and every term has `df = 2`.
Searching the full corpus for `cat dog` produces:

| Rank | Document ID | Score |
|---|---:|---:|
| 1 | 10 | 1.071445295349 |
| 2 | 20 | 0.523548346502 |
| 3 | 30 | 0.447138587823 |

Tests also prepare/load files, search each partition, and merge its top-K in test
code to verify the same ranking and scores across shard counts, including empty
partitions and cross-shard ties. This is not a production coordinator yet.

### `global_stats.bin` format (version 1)

All integers are unsigned and little-endian. Floating-point values are IEEE-754
binary64, encoded as little-endian bits. This offline format is separate from
TCP protocol frames.

| Field | Encoding |
|---|---|
| Magic | 4 literal bytes: `KRST` |
| Version | `u32`, currently `1` |
| Global document count | `u64` |
| Average document length | `f64` |
| Vocabulary size | `u64` |
| Each term, in sorted order | `u32` byte length, term bytes, `u64` document frequency |

The reader rejects bad magic/version, truncation, invalid lengths, duplicate
terms, trailing bytes, nonfinite/negative average lengths, inconsistent empty
vocabularies, zero document counts, and document frequencies outside `1..N`.
Encoded lengths are checked against the remaining file data before allocation.

## Serve a shard

```bash
./build/kretrieve prepare --input corpus.tsv --output prepared --shards 1
./build/kretrieve shard --stats prepared/global_stats.bin \
  --partition prepared/shard-0.tsv --port 0
# listening on 127.0.0.1:41237
./build/kretrieve query --port 41237 --k 10 --query "cat dog"
# 1	10	1.071445295349
# 2	20	0.523548346502
# 3	30	0.447138587823
```

`--port 0` picks a free port and prints it. `--host` defaults to `127.0.0.1`.
SIGINT/SIGTERM stop accepting, close client connections, join all workers, and
exit 0. Queued requests are dropped; already executing searches finish before exit.

**The shard has no authentication or encryption.** Keep it on loopback or a
trusted network.

The Linux server uses one level-triggered epoll event loop for nonblocking socket
I/O and a fixed query worker pool. `--workers N` sets the pool size; by default it
is the CPU count (at least one), capped at `--max-connections` (default 64).
Workers share the immutable index without locks and never access client sockets.
Idle clients and slow readers do not occupy a worker.

Connections are persistent and handle one request at a time, in order, including
pipelined requests. Reading pauses until the current response has been written.
Admitted jobs (queued, executing, or awaiting completion delivery) are bounded by
the connection cap; additional complete requests wait in FIFO order without
blocking the I/O loop. Each connection holds at most one request or response,
so memory scales with the connection cap and the 16 MiB maximum frame body.
Extra connections are accepted and closed immediately. Descriptor exhaustion
temporarily pauses accepting while existing connections continue to be served.

There is no idle timeout or search cancellation yet. An idle client holds its
connection slot until it disconnects; a disconnect during query execution is
observed when the response is ready. The `query` CLI still uses blocking I/O.

### Message bodies

Both bodies use the frame format in `protocol.hpp`
(`[u32 len][u8 version][u8 type][u64 req_id][body]`), little-endian. The
response echoes the request's `req_id`.

| Message | Body |
|---|---|
| `QueryRequest` (type 1) | `u32 k` (≤ 100000), `u64 budget_us`, `u32 query_len`, query bytes |
| `QueryResponse` (type 2) | `u8 status`, `u32 hit_count`, then per hit `u64 doc_id`, `f64 score` |

Status is `0` Ok, `1` BadRequest, `2` InternalError. Hits follow the ranking
contract above. `budget_us` is carried but ignored until deadlines land (day 4).

A well-framed request with a malformed body gets `BadRequest`, and the
connection stays open. An invalid frame (bad length, version, or type) or a
client-sent `QueryResponse` closes the connection.

## Non-goals

WAND/MaxScore, term-based partitioning, consensus/multi-writer replication,
service discovery, nDCG tuning, and stemming are outside the planned scope.
