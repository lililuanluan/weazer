#!/bin/bash

cd ..
make -j$(nproc)
cd -

mkdir -p bin
../llvm/build/bin/clang++ -Xclang -load -Xclang ../llvm/build/lib/libCDSPass.so -L.. -lmodel -Wno-unused-command-line-argument test.cc -o bin/test -Iinclude

export LD_LIBRARY_PATH=../

EXE=bin/test
VERSION=1
DEPTH=1
COMMUNICATIONEVENTS=10
HISTORY=0
TOTAL_RUN=1000


ENVIRONMENT='-x1 -p'$VERSION' -d'$DEPTH' -k'$COMMUNICATIONEVENTS' -y'$HISTORY' -v3'

echo "$ENVIRONMENT"


export C11TESTER=$ENVIRONMENT

$EXE

# OUTPUT="$(/usr/bin/time -f "time: %U %S" $EXE 2>&1)"

# echo "$OUTPUT"
