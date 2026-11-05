# KRetrieve

[![CI](https://github.com/nglong14/KRetrieve/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/nglong14/KRetrieve/actions/workflows/ci.yml)

A C++20 BM25 search engine with document shards, a TCP coordinator, deadlines, and bounded queues.

**Core idea:** every shard uses statistics from the *whole corpus*, so its scores are comparable. The coordinator requests each shard's top K and merges them. A complete reply matches the ranking of one unsharded index; a missed shard is reported as an incomplete reply.

## Try it

1. Build and test (C++20, CMake, POSIX; the first test build downloads GoogleTest):

   ```bash
   cmake -S . -B build -DBUILD_TESTING=ON
   cmake --build build -j
   ctest --test-dir build --output-on-failure
   ```

2. Prepare three documents, then start one shard:

   ```bash
   printf '10\tcat cat dog\n20\tdog bird\n30\tcat bird bird\n' > corpus.tsv
   ./build/kretrieve prepare --input corpus.tsv --output prepared --shards 1
   ./build/kretrieve shard --stats prepared/global_stats.bin \
     --partition prepared/shard-0.tsv --port 9000
   ```

3. In another terminal, query it:

   ```bash
   ./build/kretrieve query --port 9000 --k 10 --query "cat dog"
   ```

   Expected document order: **10, 20, 30**. `--port 0` selects and prints a free port.

## How retrieval works

1. `prepare` assigns each document to `document_id % shard_count` and writes `shard-N.tsv` plus `global_stats.bin`. The output directory must not already exist. Preparation holds the corpus in memory; a failed run may leave partial files.
2. Each shard builds an immutable inverted index from its partition. **All shards must load the statistics file from that same preparation run.** The artifacts do not carry a corpus identity check.
3. Search uses BM25 (`k1 = 1.2`, `b = 0.75`) with global document count, document frequency, and average length. Queries match any term; repeated terms count once. Scores sort descending, with document ID ascending on ties.
4. The coordinator sends a query to every shard, then merges their top K. If a shard fails or misses the deadline, its hits are omitted and its position in `--shards` appears in the incomplete result. Optional static replicas recover early transport failures within the same deadline; a slow primary is not retried.

Input is one `unsigned_document_id<TAB>text` per line, with unique IDs. Tokenization lowercases ASCII, splits on non-alphanumeric characters, removes built-in English stopwords, and does not stem. Empty text is allowed; an empty corpus is not.

## Run three shards

Use the example `corpus.tsv` above. Start the shards in the background, then run the coordinator:

```bash
./build/kretrieve prepare --input corpus.tsv --output prepared-3 --shards 3
for s in 0 1 2; do
  ./build/kretrieve shard --stats prepared-3/global_stats.bin \
    --partition prepared-3/shard-$s.tsv --port $((9001 + s)) &
done
./build/kretrieve coordinator --port 8000 \
  --shards 127.0.0.1:9001,127.0.0.1:9002,127.0.0.1:9003
```

In another terminal, run `./build/kretrieve query --port 8000 --k 10 --query "cat dog"`.

For crash failover, start one additional shard process per partition with the same `global_stats.bin`, then pass its addresses in matching order with `--replicas host:port,...`. Each primary/replica pair is one logical shard. If a primary connection fails or returns a malformed reply, the coordinator tries its replica once with the same request ID and remaining budget. It logs `failover shard=<index> result=<success|failure> failover_ms=<elapsed>` on stderr. If both fail, the logical index appears once in `missing_shards`. Replication is static and covers crash-stop failures; it does not provide automatic discovery, consensus, or protection from mismatched corpus files.

The coordinator's `--budget-us` defaults to 1 second when the client sends 0; the benchmark uses 200 ms. A missing shard still produces an `Ok` reply with a missing-shard list, which the query CLI prints on stderr. Requests that exhaust their budget while queued, or arrive when the worker pool and queue are full, return `Overloaded`.

The Linux server uses epoll for socket I/O and a fixed worker pool for search. Defaults are 64 maximum connections and 16 queued requests. Shards have no authentication or encryption: use loopback or a trusted network. The wire format is defined in [src/protocol.hpp](src/protocol.hpp).

## Benchmarks

The sweep uses a seeded synthetic corpus (100,000 documents), three shards, an 8-worker coordinator with a 16-request queue, and 10 seconds per offered rate. The `slow` run adds 20–100 ms to 10% of searches **on each shard**. All processes run on one machine.

| Run | Offered QPS | p99 of `Ok` replies | Overloaded |
|---|---:|---:|---:|
| Baseline | 250 | 8.88 ms | 0% |
| Baseline | 500 | 34.59 ms | 0% |
| Baseline | 1,000 | 46.43 ms | 22.15% |
| Slow | 100 | 101.29 ms | 0% |
| Slow | 500 | 150.72 ms | 22.8% |

**Read p99 with the overload rate.** The load generator schedules requests at the offered QPS and measures latency from each scheduled time, including client-side wait. Percentiles include only `Ok` replies; overload percentages include *all* requests. At 1,000 offered QPS, baseline serves 7,785 of 10,000 requests. By 4,000 offered QPS, it serves 7,301 and rejects 81.75%: successful throughput has levelled off while rejection grows.

**Slow shards drive the tail.** A complete query waits for its slowest shard. The injected delay raises p99 even at 100 QPS and causes overload at 500 QPS. These runs show latency and load shedding; they do not measure search relevance or shard-failure recovery. Treat the numbers as comparisons for this machine, not production capacity.

![Latency and failure rate against offered load](bench/latency.png)

Run the sweep and rebuild the chart:

```bash
scripts/bench.sh baseline
scripts/bench.sh slow --latency-ms 20-100 --latency-prob 0.1
python3 -m venv .venv
.venv/bin/pip install -r scripts/requirements.txt
.venv/bin/python scripts/plot.py
```

Results: [bench/summary.csv](bench/summary.csv) and one row per request in `bench/results/` (`seq,due_us,latency_us,status,missing,hits`).

## Build and release notes

CI runs normal, ASan/UBSan, and TSan builds. Use separate build directories with `-DKRETRIEVE_ASAN=ON` or `-DKRETRIEVE_TSAN=ON`; see [CONTRIBUTING.md](CONTRIBUTING.md).

[GitHub Releases](https://github.com/nglong14/KRetrieve/releases) provides the executable archive and `SHA256SUMS`. The initial supported runtime is **Ubuntu 24.04 x86-64** with standard glibc and libstdc++; the C++ library is not packaged as an SDK.
