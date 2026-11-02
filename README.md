# KRetrieve

[![CI](https://github.com/nglong14/KRetrieve/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/nglong14/KRetrieve/actions/workflows/ci.yml)

A C++20 sharded BM25 retrieval engine in progress. An offline `prepare` step
partitions a corpus and computes global statistics. Each shard serves one
partition over TCP, and a scatter-gather coordinator fans queries out to every
shard under a deadline. It merges the top-K from the shards that answer and
flags the result incomplete when some do not.

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
with `KRETRIEVE_ASAN`. CI runs both sanitizer builds. If TSan aborts with
"unexpected memory mapping", the kernel's address randomization is too high for
it; run the tests with `setarch "$(uname -m)" -R ctest --test-dir build-tsan`.

Formatting, static analysis, CI checks and the release process are described in
[CONTRIBUTING.md](CONTRIBUTING.md).

## Download a release

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
partitions and cross-shard ties. The coordinator (below) does this merge over TCP.

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

The server records when it reads each request. A shard drops a request whose
`budget_us` has run out by the time a worker picks it up: it closes the
connection without replying, because the coordinator has already stopped
waiting. A `budget_us` of 0 means no deadline. There is no idle timeout: an idle
client holds its connection slot until it disconnects.

### Message bodies

Both bodies use the frame format in `protocol.hpp`
(`[u32 len][u8 version][u8 type][u64 req_id][body]`), little-endian. The
response echoes the request's `req_id`.

| Message | Body |
|---|---|
| `QueryRequest` (type 1) | `u32 k` (≤ 100000), `u64 budget_us`, `u32 query_len`, query bytes |
| `QueryResponse` (type 2) | `u8 status`, `u32 hit_count`, then per hit `u64 doc_id`, `f64 score`, then `u32 missing_count`, then per missing shard `u32 shard_index` |

Status is `0` Ok, `1` BadRequest, `2` InternalError. Hits follow the ranking
contract above. `budget_us` is the time budget in microseconds (see the
coordinator section). A non-empty missing-shard list means the result is
incomplete; shards always send an empty list.

A well-framed request with a malformed body gets `BadRequest`, and the
connection stays open. An invalid frame (bad length, version, or type) or a
client-sent `QueryResponse` closes the connection.

## Run a coordinator

```bash
./build/kretrieve prepare --input corpus.tsv --output prepared --shards 3
for s in 0 1 2; do
  ./build/kretrieve shard --stats prepared/global_stats.bin \
    --partition prepared/shard-$s.tsv --port $((9000 + s)) &
done
./build/kretrieve coordinator --port 8000 \
  --shards 127.0.0.1:9000,127.0.0.1:9001,127.0.0.1:9002
./build/kretrieve query --port 8000 --k 10 --query "cat dog"
# same output as the single-shard example above
```

The coordinator speaks the same protocol as a shard, so `query` works against
either. It reuses the shard's epoll front end (`--host`, `--max-connections`,
`--workers` mean the same thing). For each query it:

1. assigns its own `req_id` (an atomic counter) and sends it to every shard;
   a reply with any other `req_id` counts as a failed shard. The client's own
   `req_id` is echoed back to the client.
2. sets a deadline of receive time + `budget_us` (0 means a 1-second default;
   budgets are capped at 60 seconds) and forwards the remaining budget to each
   shard.
3. fans out through a fixed thread pool of `shards × workers` threads, so a
   full set of workers never waits for a fan-out thread. Each task opens a new
   TCP connection, sends the request, and reads the reply. Connecting and
   reading both stop at the deadline. The task then closes its socket, which
   cancels the shard request.
4. merges per-shard top-K with a size-K min-heap using the same ranking
   contract (score desc, document ID asc), so results match an unsharded index.

A shard that fails (connect refused, closed connection, mismatched reply,
non-Ok status) or misses the deadline is left out. The reply is still `Ok`,
carries the other shards' hits, and lists the missing shards by their position in
`--shards`. The `query` CLI prints such a result as usual and adds
`incomplete: missing shards 1,2` on stderr:

```bash
./build/kretrieve query --port 8000 --k 10 --query "cat dog" --budget-us 200000
# incomplete: missing shards 1    (stderr, when shard 1 is down or too slow)
```

The failure model is crash-stop: a shard either answers correctly or does not
answer in time. Replicas and hedged requests are planned next.

## Non-goals

WAND/MaxScore, term-based partitioning, consensus/multi-writer replication,
service discovery, nDCG tuning, and stemming are outside the planned scope.
