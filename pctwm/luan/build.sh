#!/bin/bash

set -euo pipefail

# Usage: ./build.sh <benchname> <N>
# Examples:
#   ./build.sh mp 5          -> builds bin/mp(5) from mp-n.{cc|c}
#   ./build.sh long-assert 7 -> builds bin/long-assert(7) from long-assert.{cc|c}
#   ./build.sh n1-val 6      -> builds bin/n1-val(6) from mp.{cc|c}

if [[ $# -ne 2 ]]; then
	echo "Usage: $0 <benchname> <N>"
	echo "  benchname: mp | long-assert | n1-val"
	exit 1
fi

BENCHNAME="$1"
N="$2"

# Basic validation for N (must be an integer)
if ! [[ "$N" =~ ^[0-9]+$ ]]; then
	echo "Error: N must be an integer. Got: $N"
	exit 1
fi

mkdir -p bin

# Map benchname to source file base and output name (following existing rules)
SRC_BASE=""
OUT_NAME=""
case "$BENCHNAME" in
	mp)
		# According to existing script: mp-n.{cc|c} -> bin/mp(N)
		SRC_BASE="mp-n"
		OUT_NAME="mp"
		;;
	long-assert)
		# long-assert.{cc|c} -> bin/long-assert(N)
		SRC_BASE="long-assert"
		OUT_NAME="long-assert"
		;;
	n1-val)
		# mp.{cc|c} -> bin/n1-val(N)
		SRC_BASE="mp"
		OUT_NAME="n1-val"
		;;
	*)
		echo "Unknown benchname: $BENCHNAME"
		echo "Supported: mp | long-assert | n1-val"
		exit 1
		;;
esac

# Choose source file and compiler based on extension availability
COMPILER_CXX="../llvm/build/bin/clang++"
COMPILER_C="../llvm/build/bin/clang"
PLUGIN_OPTS=(-Xclang -load -Xclang ../llvm/build/lib/libCDSPass.so)
LINK_OPTS=(-L.. -lmodel -Wno-unused-command-line-argument)

SRC_FILE=""
COMPILER=""
if [[ -f "${SRC_BASE}.cc" ]]; then
	SRC_FILE="${SRC_BASE}.cc"
	COMPILER="$COMPILER_CXX"
elif [[ -f "${SRC_BASE}.c" ]]; then
	SRC_FILE="${SRC_BASE}.c"
	COMPILER="$COMPILER_C"
else
	echo "Source file not found for bench '$BENCHNAME'. Tried: ${SRC_BASE}.cc and ${SRC_BASE}.c"
	exit 1
fi

OUT_PATH="bin/${OUT_NAME}(${N})"

echo "Building $OUT_PATH from $SRC_FILE ..."
set -x
"$COMPILER" "${PLUGIN_OPTS[@]}" -DN="$N" -pthread "${LINK_OPTS[@]}" "$SRC_FILE" -o "$OUT_PATH" -Iinclude
set +x

echo "Done: $OUT_PATH"
