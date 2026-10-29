# AI column boundary investigation

Local passing tests did not establish complete correctness. The follow-up
review reproduced the defects below on the preceding implementation. The
platform supplied no error message or case results for its failed executions,
so their exact cause is still unconfirmed.

## Confirmed defects and fixes

1. **Blocker: result assignment protection could be bypassed.** Updates through
   an updatable view were checked against the view rather than its underlying
   AI column. A business BEFORE trigger could also assign NEW.result and leave
   a forged value. Write checks now follow the view's base column mapping, and
   trigger execution rejects changes to AI result/state fields by business
   triggers. The generated AI triggers keep their internal write path. Users
   cannot create triggers in the reserved internal namespace. Regression tests
   include nested/aliased views, transaction rollback after rejection, and an
   ordinary-column trigger that leaves JSON AI results intact.
2. **Major: dependency comparisons ignored input changes under collation.**
   Updating `lower` to `LOWER`, or `text` to `text ` under a case-insensitive
   collation, left the previous AI result unchanged. Generated invalidation
   predicates now use binary comparisons, preserving NULL-safe equality while
   observing the exact changed text passed to the model.
3. **Major: ordinary ALTER RENAME returned failure after the rename committed.**
   The post-DDL installer queried the original table name and returned error
   1146. It now resolves the post-DDL database and table names.

## Validation on the rebuilt binary

- 14 new boundary/fault regressions passed, including HTTP 429/503, response
  loss after upstream completion with an explicitly counted duplicate,
  out-of-order responses, deletion during inference, strict view reads,
  dependency changes, and view/trigger write protection.
- Five supplied AI column mysqltests and all 11 earlier integration tests passed.
- A committed leased task survived a process restart and completed after its
  lease naturally expired. The task was then removed.
- 41 file SQL checks, its supplied mysqltest, four delivery regressions, and
  two supplied AI function mysqltests passed on that same binary.

The native build and link used the established Linux x86_64 toolchain and
dependencies. Model traffic used local deterministic services. Supplied SQL
and expected results were unchanged; file fixture setup used local transport
instead of SSH/SCP. This does not reproduce the private platform environment,
its hidden cases, or the optional gateway performance evaluation.

Binary and source hashes are in
[ai-column-boundary-validation.json](ai-column-boundary-validation.json).
The previous file deployment fixes are described in
[file-sql-delivery-validation.md](file-sql-delivery-validation.md).

Run the new regressions against a test instance with:

```bash
SEEKDB_PORT=2881 python3 unittest/sql/engine/expr/test_ai_column_edge_cases.py
```

The suite creates and removes isolated test databases/models, uses an ephemeral
local HTTP service, and requires PyMySQL. It does not use real model credentials.
