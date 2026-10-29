# Asynchronous AI columns

AI columns materialize `AI_COMPLETE` and `AI_EMBED` results inside seekdb. They
support MySQL mode and asynchronous generation. Existing model and endpoint
registration remains unchanged.

```sql
CREATE TABLE documents (
  id BIGINT PRIMARY KEY,
  content VARCHAR(2000),
  tags JSON AI COLUMN (
    AI_COMPLETE('text_model', CONCAT('Classify: ', content))
  ) WITH (sync_mode='async', max_retries=2),
  embedding VECTOR(4) AI COLUMN (
    AI_EMBED('embedding_model', content)
  ) WITH (sync_mode='async', max_retries=2)
);

INSERT INTO documents(id, content) VALUES (1, 'A document');
SHOW AI COLUMNS FROM documents;

ALTER TABLE documents RETRY AI COLUMN tags
  WHERE status = 'FAILED' AND id = 1;

SET SESSION ai_column_strict_read = ON;
SELECT tags FROM documents;
```

Definitions can also be added or changed with `ALTER TABLE ... ADD COLUMN` and
`ALTER TABLE ... MODIFY COLUMN`. Definitions, dependencies, configuration and type
instructions are persisted in schema comments and `oceanbase.__all_ai_column`.
The normalized expression includes the type instruction sent to the model.
AI columns cannot depend on another AI column or be primary keys. Input
expressions must use supported deterministic scalar operators/functions.

## Transaction and publication rules

Internal triggers invalidate affected results and upsert durable tasks in the
business transaction. Inserts, changes to dependencies and changes to primary
keys enqueue work; unrelated updates retain ready results. Rollback removes the
tasks and restores the row and its indexes. Composite and implicit primary keys
are supported. Business SQL cannot assign an AI result or its hidden state.

The background workers read committed tasks, acquire each attempt with a
conditional lease update, and invoke existing AI functions outside the business
transaction. There are four workers and leases last 120 seconds. An expired
lease is eligible for recovery after interruption or restart. An interrupted
external request can be repeated; the implementation does not claim exactly-once
inference.

Requests carry a row request identifier and definition version. Publication
checks both against the current row, so an older response cannot overwrite a
newer dependency value or changed definition. Tasks for one row and change are
published together after all members reach a terminal state. Successful members
become `READY`; exhausted or invalid members become `FAILED`. Terminal task rows
are deleted in the publication transaction. Failed publication is deferred so
other groups can proceed; groups belonging to dropped tables are discarded.

String, integer, numeric, JSON and dense-vector results are checked before
publication. JSON must parse, integers must fit their declared width, floating
values must be finite and vectors must have the declared dimension and finite
elements. Vector writeback uses validated native vector literals. Native DML
maintains ordinary and vector indexes when results are invalidated or published.

## Read and retry semantics

The default read returns `NULL` for pending or failed results. Readiness is stored
separately, so a valid JSON null does not represent unfinished work.

`ai_column_strict_read` defaults to `OFF`. When enabled, every AI column explicitly
referenced by a SELECT, UPDATE or DELETE must be ready for the entire table,
including rows excluded by its predicate. Ordinary columns and other fully ready
AI columns remain readable. The check shares the user transaction, including
uncommitted invalidation. Strict statements bypass the plan cache so readiness is
checked on each execution.

`RETRY AI COLUMN` affects one named AI column and matching rows, including ready
rows. Unquoted `status` is a retry-only state pseudo-column; backtick-quoted
`status` refers to the business column. Automatic retries are bounded by
`max_retries` (0–10). A retry cannot be combined with another ALTER action.

## Validation

The five supplied mysqltests are under
`tools/deploy/mysql_test/test_suite/ai_column/`. Their SQL and expected output
must remain unchanged when testing this implementation.

Additional native integration coverage is in
`unittest/sql/engine/expr/test_ai_column_integration.py`. With PyMySQL installed
and a running local server, run:

```sh
SEEKDB_PORT=2881 python3 unittest/sql/engine/expr/test_ai_column_integration.py
```

It creates unique temporary databases/models and an ephemeral local HTTP mock.
It covers definition changes during inference, composite and implicit row keys,
rollback without external calls, expired ownership recovery, write protection,
strict reads inside transactions, partial failure and drop/recreate isolation.
No real model credentials are required.
