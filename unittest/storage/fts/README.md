# Full-text parser regression checks

These checks supplement the unchanged `tools/benchmark/fts_large_bench*` benchmark.
They never alter its inputs, SQL, timing, scoring, or fixed baseline.

`test_fts_hot_path.cpp` checks all Unicode scalars in UTF-8, UTF-16BE and UTF-16LE
against the classifier from official commit
`5bce9e06d1281347ad61344e4b01fab9158c58a2`, plus malformed inputs. It also checks
IK list-cell lifetimes with a freeing allocator, arbitration at inline-array
capacity boundaries, BENG token ownership across reset/destruction, and dictionary
cleanup when each allocation fails in turn.

The suite also compares stopword matching with an independent implementation
of the original charset-conversion and hash-set path: 121,140 comparisons across
six collations cover ASCII case combinations, trailing spaces, embedded NUL,
malformed bytes, non-ASCII fallback and deterministic arbitrary ASCII strings.
It checks initialization, destruction and reinitialization, plus frequency
updates under collisions and binary/case-insensitive collations.

After compiling the test executable, `--test-stopwords` runs just the stopword
and frequency checks. `--benchmark-stopwords` measures thread CPU time in ABBA
order for ASCII hits, ASCII misses and fallback inputs separately; it checks
that hit counts agree. `--benchmark-word-map` measures the frequency update.
These operation-level measurements do not measure complete SQL query latency.

Build this checkout with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, including the
`seekdb` and `oceanbase` targets, then run:

```bash
python3 unittest/storage/fts/run_fts_cpp.py \
  --build-dir /path/to/build --output /tmp/fts-cpp-check
```

The runner uses that build's compiler and ABI flags, and extracts the unmodified
reference classifier from a checksum-verified fixture copied verbatim from that
official commit. This also works in the destination repository, whose initial
competition import has a separate history. Test objects and logs go to `--output`.

`check_fts_sql.py` compares complete, canonicalized TOKENIZE results, including
word counts and document lengths, and complete MATCH document-ID lists with
relevance values rounded to ten decimal places. It also
checks both IK modes, other built-in parsers, boolean/natural-language searches,
ordered top-k results, updates, deletes and transaction rollback. It uses only
Python's standard library and a MySQL-compatible command-line client.

Start a dedicated instance of the official base build, then record the reference:

```bash
python3 unittest/storage/fts/check_fts_sql.py --port 2881 \
  --write /tmp/fts-reference.json --output /tmp/fts-before
```

Start the optimized build with the same resource settings and compare:

```bash
python3 unittest/storage/fts/check_fts_sql.py --port 2881 \
  --compare /tmp/fts-reference.json --output /tmp/fts-after
```

Both commands drop and recreate only `fts_hot_path_regression`. Use `--mysql` to
specify the client executable. Add `--benchmark-results` to both commands after
running the official large benchmark; this also compares complete hit ID sets
from all four benchmark query predicates, beyond the benchmark's hit counts.

The integrated mysqltest case is
`tools/deploy/mysql_test/test_suite/fts_index/t/parser_hot_path.test`, with its
base-generated expected result in `r/mysql/parser_hot_path.result`. It uses the
separate `fts_hot_path_mysqltest` database.
