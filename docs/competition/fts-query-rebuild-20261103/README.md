# Full-text query merge and relevance filtering

This candidate is based on `6a94f77360c3f1e70cec626f44d8e7769181c14b`. It retains the submitted tokenizer and index-building implementations and changes one production file.

## Problem and change

A retained query CPU profile attributed work to binary string comparison and generic charset dispatch during document-ID merging. For VARCHAR IDs with exactly CS_TYPE_BINARY, the merge comparator now chooses a null-first byte comparator once at initialization. Trailing spaces remain significant; empty strings, embedded zero bytes, variable lengths and all byte values are supported. Other types and collations retain the existing comparator. No result caching or benchmark-specific constants are introduced.

The same profile also identified per-row boolean relevance evaluation. When the filter's exact evaluator is the standard double-to-boolean function and its only child is the already projected relevance expression, filtering now uses that evaluator's identical nonzero test. The filter remains unevaluated for any other consumer to compute normally; arbitrary predicates retain the original evaluation path.

## Validation

- Full seekdb and liboceanbase builds succeeded with the retained RelWithDebInfo / -O2 settings.
- 2,405,754 comparator checks against the original datum comparator passed, including null ordering, binary values, text collation fallback and integer fallback.
- 936 complete SQL result comparisons passed, including tokenization, complete MATCH results, relevance to ten decimal places, top-k, DML and rollback cases.
- 240 parser-configuration result/error comparisons passed, including 183 expected errors.
- 21 additional boolean-filter cases passed by comparing the complete ordered ID and rounded relevance output byte-for-byte: plain MATCH, thresholds, equality to zero, AND, OR, NOT, and a no-hit term. The SQL fixture is included; run it with a batch MySQL client on both versions after benchmark setup and compare the complete output.
- Four fresh-instance full benchmark runs in submitted/candidate/candidate/submitted order. All runs retained, no SELECT 1 subtraction. Official data, SQL, timing, scoring and baseline files unchanged and checked by SHA-256.

## Local performance

Relative to the submitted version, category-average elapsed-time improvements from the two-run means:

| Category | Improvement |
|---|---:|
| Build | -2.345% |
| Tokenize | +5.662% |
| Query | +12.490% |
| Equal-category composite | +5.269% |

These are local version-to-version comparisons, not official scores, a guarantee of stable improvement, or a full-score claim. Build/tokenize paths are unchanged; movement in those categories can reflect environmental variance. Earlier comparator-only experiments showed inconsistent composite results, so that variant was not selected alone. Raw final-run reports and summary are included.

## Reproduce

After building this branch, run `python3 unittest/storage/fts/run_sparse_id_compare.py --build-dir <build> --output <evidence>`. Run the unchanged `tools/benchmark/fts_large_bench.sh` against a dedicated instance, with ROWS=20000, BATCH=500, ROUNDS=3000, QUERY_ROUNDS=200, SAMPLES=3, WARMUP=30 and SKIP_LOAD=0, using a new report file for each run.

The scoring code defaults to `tools/benchmark/fts_large_bench_baseline.json`; `--baseline` can select another file. Three categories are averaged equally, then the composite is clamped at zero and 50% for the 100-point performance score (50-point question weight). The repository baseline does not reproduce the two previously returned platform scores when applied to their reported timings. The actual platform baseline and scoring invocation require verification; no scoring files are changed here.
