#!/bin/bash

# UNROLL=3

# COMMON_FLAGS=" --disable-estimation --disable-race-detection --disable-sr --disable-ipr --count-distinct-execs  --disable-spin-assume  --unroll=$UNROLL   --no-unroll=main  "
COMMON_FLAGS=" --disable-estimation --disable-race-detection --disable-sr --disable-ipr --count-distinct-execs    "

# verification FLAGS
VERIF_FLAGS=$COMMON_FLAGS

# random FLAGS
RANDOM_FLAGS=" --fuzz --fuzz-max=${N}  --mutation-policy=no-mutation $COMMON_FLAGS "
# fuzzing flags

# fuzz mode

COMMON_FZ_FLAGS=" --fuzz  --fuzz-max=${N}   --use-queue  --mutation-policy=revisit  --is-interesting=new  --fuzz-value-noblock=true --num-mutation=3 --insert-rand=30 -fuzz-corpus=20  $COMMON_FLAGS  "

# no bias
FUZZ_FLAGS_no=" ${COMMON_FZ_FLAGS}  "

# only prioritize new value sequences
FUZZ_FLAGS_star=" ${COMMON_FZ_FLAGS} --prio-new-val  --schedule-policy=wfr  "

# only prioritize old writes
FUZZ_FLAGS_plus=" ${COMMON_FZ_FLAGS}  --prio-stale-stores  --schedule-policy=wfr  "

# only prioritize backward revisits
FUZZ_FLAGS_ddag=" ${COMMON_FZ_FLAGS}  --prio-back-rev  --schedule-policy=wfr   "

# both old store and new val
FUZZ_FLAGS_plustar=" ${COMMON_FZ_FLAGS}  --prio-new-val  --prio-stale-stores   --schedule-policy=wfr  "

# both back-rev and new val
FUZZ_FLAGS_ddagstar=" ${COMMON_FZ_FLAGS}   --prio-new-val --prio-back-rev   --schedule-policy=wfr  "

#  - when starting from an empty graph: add maximally (reads read from the end of rfs, writes are added at the end of cos)
#  - when mutating a graph: weight = 1
FUZZ_FLAGS_rfco=" ${COMMON_FZ_FLAGS} --add-max=empty  --schedule-policy=wfr "

#  - when starting from an empty graph: add maximally (reads read from the end of rfs, writes are added at the end of cos)
#  - when mutating a graph: weight = 1 + isNewValue
FUZZ_FLAGS_rfcostar=" ${COMMON_FZ_FLAGS} --add-max=empty --prio-new-val --schedule-policy=wfr "

#  - when starting from an empty graph: add maximally (reads read from the end of rfs, writes are added at the end of cos)
#  - when mutating a graph: weight = 1+isNewValue+isBackwardRevisit
FUZZ_FLAGS_rfcostarddag=" ${COMMON_FZ_FLAGS} --add-max=empty --prio-new-val --prio-back-rev --schedule-policy=wfr "

#  - when starting from an empty graph: pick rf/co randomly
#  - when mutating a graph: weight = 1
#  - when starting from a mutated graph: add maximally
FUZZ_FLAGS_3ph=" ${COMMON_FZ_FLAGS} --add-max=mutated   --schedule-policy=wfr  "

#  - when starting from an empty graph: pick rf/co randomly
#  - when mutating a graph: weight = 1 + isNewValue
#  - when starting from a mutated graph: add maximally
FUZZ_FLAGS_3phstar=" ${COMMON_FZ_FLAGS} --add-max=mutated --prio-new-val   --schedule-policy=wfr  "

#  - when starting from an empty graph: pick rf/co randomly
#  - when mutating a graph: weight = 1+isNewValue+isBackwardRevisit
#  - when starting from a mutated graph: add maximally
FUZZ_FLAGS_3phstarddag=" ${COMMON_FZ_FLAGS} --add-max=mutated --prio-new-val  --prio-back-rev   --schedule-policy=wfr  "
