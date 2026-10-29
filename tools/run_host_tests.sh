#!/bin/sh
# components/core の単体テストを Mac/Linux 上で実行する
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$ROOT/test/host" -B "$ROOT/build/host-tests" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$ROOT/build/host-tests" >/dev/null
"$ROOT/build/host-tests/core_tests"
