# KRetrieve

A C++20 sharded BM25 retrieval engine in progress. Day 1 provides offline corpus
preparation and immutable, in-process search. TCP shard serving and the
scatter-gather coordinator are not implemented yet.

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
with `KRETRIEVE_ASAN`. Day 1 does not add concurrent execution.

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

## Non-goals

WAND/MaxScore, term-based partitioning, consensus/multi-writer replication,
service discovery, nDCG tuning, and stemming are outside the planned scope.
