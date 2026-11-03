#!/usr/bin/env python3
"""Generate a synthetic corpus and query set with a Zipf-distributed vocabulary.

Ranking quality is not what KRetrieve benchmarks measure, so a seeded synthetic
corpus beats a download: it is reproducible and needs no network.
Writes <out>/corpus.tsv (id<TAB>text) and <out>/queries.txt (one query per line).
"""

import argparse
import itertools
import os
import random


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="bench/data")
    parser.add_argument("--docs", type=int, default=100_000)
    parser.add_argument("--vocab", type=int, default=50_000)
    parser.add_argument("--doc-len", type=int, default=50, help="mean terms per document")
    parser.add_argument("--queries", type=int, default=2_000)
    parser.add_argument("--zipf", type=float, default=1.0, help="Zipf exponent")
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    words = [f"t{rank}" for rank in range(1, args.vocab + 1)]
    cumulative = list(itertools.accumulate(1 / rank**args.zipf for rank in range(1, args.vocab + 1)))

    def draw(count):
        return rng.choices(words, cum_weights=cumulative, k=count)

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "corpus.tsv"), "w") as corpus:
        for doc_id in range(1, args.docs + 1):
            length = rng.randint(args.doc_len // 2, args.doc_len * 3 // 2)
            corpus.write(f"{doc_id}\t{' '.join(draw(length))}\n")
    with open(os.path.join(args.out, "queries.txt"), "w") as queries:
        for _ in range(args.queries):
            queries.write(" ".join(draw(rng.randint(2, 3))) + "\n")
    print(f"wrote {args.docs} documents and {args.queries} queries to {args.out}")


if __name__ == "__main__":
    main()
