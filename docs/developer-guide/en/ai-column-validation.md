# Native validation record

The cumulative implementation was built and tested on Linux x86_64 against
one native seekdb binary. Verification finished at `2026-10-29T08:56:17.735226+00:00`.

| Check | Result |
| --- | --- |
| Native CMake build and link | PASS |
| Supplied AI column mysqltests | 5/5 PASS |
| Additional integration regressions | 11/11 PASS |
| Committed leased task recovery after process restart | PASS |
| Task-two file SQL checks | 38/38 PASS |
| Supplied task-two file SQL mysqltest | PASS |
| Supplied task-one AI function mysqltests | 2/2 PASS |

The five AI column tests cover DDL, transactional invalidation, background
execution, retries and strict reads, and vector index consistency. Their SQL
and expected results were left unchanged. The file SQL runner substitutes
local fixture transport for SSH/SCP; its supplied SQL and expected results
remain unchanged. AI requests use local HTTP mocks.

The integration suite covers definition replacement during inference, SHOW,
composite and implicit row keys, rollback, expired claims, direct-write
protection, strict reads inside transactions, partial failure, drop/recreate,
integer storage bounds, NULL input, and repeated vector regeneration through
BEFORE triggers with ANN queries between updates.

The final fixes place strict checks after statement startup and before physical
execution, plan fresh vector IDs for possible BEFORE-trigger vector changes,
and recognize wrapped ID generation expressions in new-row projectors. Fresh
vector IDs prevent repeated updates from inserting the same vector-index
deletion key.

Binary SHA-256: `3302ee2e3438568dbf433edaed83d9873a9e75e3db33d29e5b49008f563a6634`.
Source and official fixture hashes are in [ai-column-validation.json](ai-column-validation.json).
Usage and the integration command are in [ai-column.md](ai-column.md).

This record reports local native validation; it is not an online competition
platform result or submission.
