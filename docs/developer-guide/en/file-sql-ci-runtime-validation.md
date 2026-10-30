# CI runtime archive validation — 2026-10-30

The mysqltest runtime archive previously contained `seekdb` and the client tools,
but omitted `libseekdb_file_parquet.so`. A separate runner cannot use the
producer's absolute build path to find that decoder. The workflow now packages
the decoder beside the executable and fails immediately if the decoder is missing.

All six tests in `unittest/sql/engine/basic/test_file_sql_delivery.py` passed:

- The farm build entry selects the supported release mode.
- Manual DEB staging includes the decoder and rejects a missing decoder.
- CI runtime staging includes the decoder and rejects a missing decoder.
- The real loader opens a Parquet fixture after relocation, without a compiled-in
  developer library path or `LD_LIBRARY_PATH`; the missing-library control fails.

The CI archive tests execute the workflow's actual staging commands and inspect
the resulting compressed archive. The relocated-loader test used a decoder
compiled and linked against freshly downloaded EL7 Arrow/Parquet dependencies;
the existing compiler toolchain and zlib archive were reused. This was not a full
clean build of seekdb. The earlier kernel regression results remain documented
in `file-ai-review-validation.md`; this change modifies no kernel source.

This verifies the repository's CI artifact path. It does not establish which
build or deployment commands the private competition evaluator uses, nor prove
the cause of the platform's execution-failed status.
