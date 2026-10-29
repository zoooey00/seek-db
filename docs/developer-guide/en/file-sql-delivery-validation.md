# File SQL delivery investigation

The platform reported execution failure without an error message or individual
test results. The meaning of its numeric `failMsg` was not established. The
defects below were reproduced locally; which one, if any, caused that platform
run remains unconfirmed.

## Confirmed defects and fixes

1. **Major: the farm wrapper selected an unsupported build mode.** With
   `PACKAGE_TYPE` unset, `.github/script/seekdb/scripts/farm_compile.sh` invoked
   `build.sh debug --init --make`. The maintained entry point rejected `debug`
   and exited with code 2. The wrapper now defaults to `release`. An explicitly
   supplied build mode is preserved.
2. **Major: the manual DEB payload omitted the Parquet runtime decoder.**
   `package/deb/seekdb-build.sh` copied the executable without its dynamically
   loaded decoder. A relocated reader failed with `Cannot load local Parquet
   decoder`; placing the decoder in the relative library directory restored
   the read. The build now emits `libseekdb_file_parquet.so` beside `seekdb`,
   and the manual DEB payload requires and installs it into `usr/lib`.
   Existing CPack server installation already included this decoder.
3. **Major: the file DESCRIBE extension intercepted ordinary SELECT.**
   `DESCRIBE SELECT 1` returned error 1235 even though the existing EXPLAIN
   equivalent worked. The resolver now uses the existing EXPLAIN path for
   statements without a file table source. File schema descriptions retain
   their dedicated path. Regressions cover ordinary DESCRIBE and DESC with
   file SQL disabled, and DESCRIBE with file SQL enabled.

## Verification

The rebuilt Linux x86_64 CMake binary passed:

- Four delivery regressions: the farm default, DEB inclusion, rejection of a
  missing decoder, and relocated decoding without a compiled build path.
- 41 file SQL checks, including the three ordinary SQL compatibility checks.
- The supplied file SQL mysqltest, with SQL and expected results unchanged;
  fixture transport was substituted locally for SSH/SCP.
- Five supplied AI column mysqltests and 11 AI column integration regressions.
- Two supplied AI function mysqltests using local HTTP mocks.

The full compile and link used the existing verified Linux toolchain and
dependencies. This does not reproduce the platform's private clean-build,
deployment or hidden-test environment. Tests on the earlier binary remain
historical evidence; the new binary and source hashes are recorded separately
in [file-sql-delivery-validation.json](file-sql-delivery-validation.json).

## Deployment and reproduction

Build with the maintained entry point:

```bash
./build.sh release --init --make -j4
```

A build-directory bundle must include both files from
`build_release/src/observer`: `seekdb` and `libseekdb_file_parquet.so`.
Installed packages place the executable in `usr/bin` and decoder in `usr/lib`.
Copying the executable alone does not deliver Parquet support.

Run the focused deployment regressions with:

```bash
SEEKDB_PARQUET_LIBRARY="$PWD/build_release/src/observer/libseekdb_file_parquet.so" \
  python3 unittest/sql/engine/basic/test_file_sql_delivery.py
```

Set `CXX` to a supported C++ compiler if necessary. The relocation test omits
the compiled absolute decoder path and removes `LD_LIBRARY_PATH`, so the
original build directory cannot conceal a missing packaged decoder.
