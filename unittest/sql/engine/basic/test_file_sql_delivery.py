"""Focused build-entry and relocated Parquet payload regressions (Linux)."""
import base64
import io
import tarfile
import textwrap
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]


class FileSqlDeliveryTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="seekdb-file-delivery-")
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def test_farm_default_uses_supported_release_mode(self):
        workspace = self.root / "workspace"
        workspace.mkdir()
        recorder = workspace / "arguments"
        build = workspace / "build.sh"
        build.write_text('#!/bin/bash\nprintf "%s\\n" "$@" > "$WORKSPACE/arguments"\n')
        build.chmod(0o755)
        environment = {k: v for k, v in os.environ.items() if k != "PACKAGE_TYPE"}
        environment["WORKSPACE"] = str(workspace)
        script = REPO / ".github/script/seekdb/scripts/farm_compile.sh"
        subprocess.run(["bash", str(script)], env=environment, check=True)
        self.assertEqual(["release", "--init", "--make"], recorder.read_text().splitlines())

    def make_deb_inputs(self):
        source = self.root / "source"
        inputs = {
            "build_release/src/observer/seekdb": b"test-binary",
            "build_release/src/observer/libseekdb_file_parquet.so": b"test-decoder",
            "build_release/syspack_release/package.sql": b"SELECT 1;",
            "deps/3rd/home/admin/oceanbase/bin/obshell": b"test-obshell",
            "tools/import_time_zone_info.py": b"",
            "tools/import_srs_data.py": b"",
            "tools/timezone_V1.log": b"",
            "tools/default_srs_data_mysql.sql": b"",
        }
        for name in ["seekdb.service", "seekdb_systemd_start", "seekdb_systemd_stop",
                     "seekdb.cnf", "oceanbase-pre.json", "telemetry-pre.json",
                     "telemetry.sh.template"]:
            inputs["tools/systemd/profile/" + name] = b"fixture"
        for relative, content in inputs.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        maintainer = self.root / "maintainer"
        maintainer.mkdir()
        for name in ["pre_install", "post_install", "pre_uninstall", "post_uninstall"]:
            (maintainer / (name + ".sh")).write_text("#!/bin/bash\n")
        return source, maintainer

    def stage_deb(self, source, maintainer):
        script = (REPO / "package/deb/seekdb-build.sh").read_text()
        # Execute the actual payload function without invoking a full package build.
        definitions = script[script.index("function fail"):script.index("\n(( $# == 4 ))")]
        environment = {**os.environ, "TOP_DIR": str(source)}
        return subprocess.run(
            ["bash", "-euo", "pipefail", "-c",
             definitions + '\ninstall_payload "$1" 1.0 1 "$2"',
             "payload-test", str(self.root / "package"), str(maintainer)],
            env=environment, capture_output=True, text=True)

    def test_manual_deb_includes_decoder(self):
        source, maintainer = self.make_deb_inputs()
        result = self.stage_deb(source, maintainer)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        decoder = self.root / "package/usr/lib/libseekdb_file_parquet.so"
        self.assertEqual(b"test-decoder", decoder.read_bytes())

    def test_manual_deb_rejects_missing_decoder(self):
        source, maintainer = self.make_deb_inputs()
        (source / "build_release/src/observer/libseekdb_file_parquet.so").unlink()
        result = self.stage_deb(source, maintainer)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("required package input is missing", result.stderr)

    def stage_ci_runtime(self, include_decoder=True):
        workspace = self.root / "ci-workspace"
        workspace.mkdir()
        binary = workspace / "build_release/src/observer/seekdb"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"fixture-executable")
        binary.chmod(0o755)
        if include_decoder:
            (binary.parent / "libseekdb_file_parquet.so").write_bytes(b"fixture-decoder")
        clients = workspace / "deps/3rd/u01/obclient/bin"
        clients.mkdir(parents=True)
        for name in ("obclient", "mysqltest"):
            (clients / name).write_bytes(b"fixture-client")
            (clients / name).chmod(0o755)
        workflow = (REPO / ".github/workflows/seekdb.yml").read_text()
        step = workflow.split("      - name: Prepare mysqltest runtime\n", 1)[1]
        commands = step.split("        run: |\n", 1)[1]
        lines = []
        for line in commands.splitlines():
            if line and not line.startswith("          "):
                break
            lines.append(line)
        environment = {**os.environ, "SEEKDB_BINARY_PATH": str(binary),
                       "RUNNER_TEMP": str(self.root / "runner"), "GITHUB_RUN_ID": "fixture"}
        result = subprocess.run(["bash", "-c", textwrap.dedent("\n".join(lines))],
                                cwd=workspace, env=environment, capture_output=True, text=True)
        return result, workspace / "seekdb-mysqltest-runtime.tar.zst"

    def test_ci_runtime_archive_includes_decoder(self):
        result, archive = self.stage_ci_runtime()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        payload = subprocess.check_output(["zstd", "-d", "-q", "-c", str(archive)])
        with tarfile.open(fileobj=io.BytesIO(payload)) as files:
            self.assertEqual(b"fixture-executable", files.extractfile("seekdb").read())
            self.assertEqual(b"fixture-decoder", files.extractfile("libseekdb_file_parquet.so").read())

    def test_ci_runtime_rejects_missing_decoder(self):
        result, archive = self.stage_ci_runtime(include_decoder=False)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("required Parquet decoder missing", result.stdout)
        self.assertFalse(archive.exists())

    @unittest.skipUnless(os.environ.get("SEEKDB_PARQUET_LIBRARY"),
                         "set SEEKDB_PARQUET_LIBRARY to the built runtime decoder")
    def test_decoder_works_after_relocation_without_build_path(self):
        library = Path(os.environ["SEEKDB_PARQUET_LIBRARY"])
        self.assertTrue(library.is_file(), str(library))
        binary_dir = self.root / "relocated/bin"
        lib_dir = self.root / "relocated/lib"
        binary_dir.mkdir(parents=True)
        lib_dir.mkdir()
        source = self.root / "probe.cpp"
        source.write_text(r'''#include "sql/engine/basic/file_scan_parquet.h"
#include <cstdio>
#include <fcntl.h>
int main(int argc, char **argv) {
  char error[2048] = {};
  void *reader = file_parquet_open(open(argv[1], O_RDONLY), error, sizeof(error));
  if (!reader) { puts(error); return 1; }
  printf("columns=%d\n", file_parquet_column_count(reader));
  file_parquet_close(reader);
  return 0;
}
''')
        binary = binary_dir / "probe"
        command = [os.environ.get("CXX", "c++"), "-std=c++20"]
        if os.environ.get("SEEKDB_TOOLCHAIN"):
            command.append("--gcc-toolchain=" + os.environ["SEEKDB_TOOLCHAIN"])
        # Deliberately omit FILE_SQL_PARQUET_LIBRARY: a developer build path must
        # never make this packaging regression pass accidentally.
        command += ["-I" + str(REPO / "src"), str(source),
                    str(REPO / "src/sql/engine/basic/file_scan_parquet_loader.cpp"),
                    "-static-libstdc++", "-static-libgcc", "-ldl", "-pthread",
                    "-o", str(binary)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        fixture = self.root / "simple.parquet"
        fixture.write_bytes(base64.b64decode((REPO / (
            "tools/deploy/mysql_test/test_suite/file_sql/data/simple.parquet.b64")).read_bytes()))
        environment = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
        missing = subprocess.run([str(binary), str(fixture)], env=environment,
                                 capture_output=True, text=True)
        self.assertNotEqual(0, missing.returncode)
        self.assertIn("Cannot load local Parquet decoder", missing.stdout)
        shutil.copy2(library, lib_dir / "libseekdb_file_parquet.so")
        result = subprocess.run([str(binary), str(fixture)], env=environment,
                                capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("columns=1", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
