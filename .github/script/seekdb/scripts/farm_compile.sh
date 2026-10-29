#!/usr/bin/env bash
# Farm compile: build seekdb/observer in workspace.
# Expects: WORKSPACE, SEEKDB_TASK_DIR, PACKAGE_TYPE (release|sanity), REPO=server
set -e

cd "$WORKSPACE"
# Match the maintained build.sh production mode when the caller supplies none.
build_type="${PACKAGE_TYPE:-release}"
if [[ -x "$WORKSPACE/build.sh" ]]; then
  bash "$WORKSPACE/build.sh" "$build_type" --init --make
else
  echo "[farm_compile.sh] No build.sh in $WORKSPACE, skip."
fi
