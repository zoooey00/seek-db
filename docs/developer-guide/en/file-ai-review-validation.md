# File SQL and AI column follow-up review

Review of the previously published source (remote commit
`6149ebb69aef24362e9b0844b3e2812872250a31`) reproduced four defects. The added
regressions also exposed a fifth issue in repeated COPY statements. All five
are repaired in this revision.

1. **Blocker: business triggers could leave stale AI results READY.** A BEFORE
   UPDATE trigger changing a dependency after AI invalidation left the old
   result visible, even under strict reads, without creating a new task.
   Generated AI BEFORE triggers now run after business BEFORE triggers, keeping
   business trigger order intact and evaluating the final dependency values.
   Tests cover regeneration, rollback without model calls, and a business
   trigger reverting a proposed change without unnecessary regeneration.
2. **Major: RENAME TABLE stranded completed AI tasks.** Publication used the
   original cached table name. The reproduced task had completed model work
   but repeatedly failed publication while strict reads remained unavailable.
   Publication now resolves the current database/table name by immutable table
   ID and refreshes metadata. A concurrent rename failure retains the task for
   retry. Tests cover updates and manual retry after rename, plus rename during
   a model request while an unrelated replacement table takes the old name.
3. **Major: quoted CSV fields lost carriage returns.** COPY exported `a\r\nb`
   correctly, but read_csv returned `a\nb`. The reader now preserves CR, LF,
   and CRLF inside quoted fields while still recognizing record terminators.
   The earlier implementation-owned C++ regression had incorrectly expected
   normalized data; its expectation now preserves the original bytes.
4. **Major: JSON numeric boundaries were rounded before VARCHAR widening.**
   A JSONL column containing integer `18446744073709551616` and a string became
   VARCHAR but returned `18446744073709552000.0`. Streaming SAX parsing now
   retains raw numeric tokens before inference. Out-of-range integers remain
   exact VARCHAR values; signed BIGINT bounds and DOUBLE widening still work.
5. **Major: COPY reused the first target for subsequent normalized queries.**
   Repeating COPY with a different target path could report that the first
   file already existed. Compiled COPY descriptors now pin parameter values,
   as file-scan schemas already do. Repeated export/read-back cases exercise
   distinct target paths and values in one session.

Validation includes a full native build/link, 19 AI boundary tests, 6 file
boundary tests, 11 AI integration tests, 5 supplied AI column mysqltests,
restart recovery, 41 file SQL checks, its supplied mysqltest, native file-scan
unit tests, 4 delivery checks, and 2 supplied AI-function mysqltests. The
integration suites use one rebuilt binary and local deterministic model
services. Supplied mysqltest SQL/expected results were unchanged; file fixture
transport is local instead of SSH/SCP.

The platform returned no failure message or case details for the historical
failed submissions. These fixes do not establish their exact cause or guarantee
hidden-case scores. A clean private-platform environment and optional gateway
performance assessment have not been reproduced.

Hashes and stage results: [file-ai-review-validation.json](file-ai-review-validation.json).

New tests require PyMySQL and a local server with a writable secure_file_priv
test directory; they create isolated files/databases/models and clean them up:

```bash
SEEKDB_PORT=2881 python3 unittest/sql/engine/basic/test_file_sql_edge_cases.py
SEEKDB_PORT=2881 python3 unittest/sql/engine/expr/test_ai_column_edge_cases.py
```
