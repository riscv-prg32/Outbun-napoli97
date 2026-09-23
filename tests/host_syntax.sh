#!/usr/bin/env bash
# Strict host compile of the cartridge source against the stub PRG32 header.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
"$CC_BIN" -std=c99 -Wall -Wextra -Werror -Wno-unused-function -fsyntax-only -DOUTBUN_HOST -I"$ROOT/tests/stub" -I"$ROOT/src" "$ROOT/src/game.c"
echo "host syntax: OK"
