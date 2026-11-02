# Static zlib layout and clean-build validation

## Confirmed failure

The Parquet decoder hardcoded `${DEP_DIR}/lib64/libz.a`. The standard EL7
`devdeps-zlib-static` dependency installs that archive under `${DEP_DIR}/lib`.
A fresh build of the previously published source failed before linking the
decoder with `No rule to make target .../lib64/libz.a`. The earlier custom local
dependency directory masked this mistake. The path originated in the task-two
implementation and remained in subsequent cumulative submissions.

CMake now resolves the static archive in the selected dependency prefix's
`lib` and `lib64` directories, rejecting missing archives during configuration.
It does not silently substitute a system library.

## Validation

- Complete release build and seekdb link passed with freshly downloaded standard
  C/C++ dependency packages and no reused build directory or compiled objects.
- Nine delivery tests passed, including both zlib layouts, a missing-library
  case, real decoder relocation, and CI/DEB payload checks.
- The newly built executable initialized a new database directory successfully.
- All five supplied AI-column mysqltests passed without changing their statements
  or expected results: DDL, DML, worker, retry/strict read, and vector index.
- The supplied file-SQL mysqltest and all 41 file-SQL integration checks passed.
- Both AI-function mysqltests (`ai_prompt`, `ai_function_enhancement`) passed
  using the existing local deterministic Mock service.

The source snapshot was exported from the local Git tree previously verified
identical to published commit `56f3fc25f16d8eae6c07159724eb09617af9b843`, then
patched only for this build fix and its delivery tests. Direct GitHub cloning
failed locally with a TLS transport error. The build reused the exact pinned
Rust 1.98.1 compiler and downloaded crate cache, but rebuilt Rust outputs in the
new build directory. No private competition runner logs were available: this is
a reproduced source-build defect, not a claim of a confirmed platform score or
proof that no other platform-specific failure remains.

Validated executable SHA-256: `8a9853c02c6966a9586b9aaa82cf83dda398ad94d728a5ac7604b9ee17415f8a`.
