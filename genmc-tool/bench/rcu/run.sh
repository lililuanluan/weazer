#!/bin/bash

TOOL=../../genmc

RCU_DIR=.
RCU_VERSION_DIR=$RCU_DIR/valtree/v3.0

$TOOL --disable-function-inliner --disable-estimation --unroll=5 -- -I$RCU_VERSION_DIR -std=c11 $RCU_DIR/valtree/litmus_v3.c