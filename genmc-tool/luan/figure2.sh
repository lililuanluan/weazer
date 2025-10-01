#!/bin/bash

source ./benchmarks.sh


function run_benchmark {

    local benchname=$1
    local mode=$2
    local benchdir=${benchmarks_correct[$benchname]}
    local flag=${flags_correct[$benchname]}
    local total=0
    local total_complete=0
    local total_blocked=0
    local repeat_count=$REPEAT
    local modestr=$3

    local cnt=0
    for ((i = 0; i < repeat_count; i++)); do

        dumpname="out/coverage/${benchname}"
        # cmd="../genmc --dump-coverage=${dumpname}"

        if [[ "$mode" == "$FUZZ_FLAGS" ]]; then
            dumpname="${dumpname}-fuzz-${i}"
        else
            dumpname="${dumpname}-rand-${i}"
        fi
        cmd="../genmc --dump-coverage=${dumpname} --dump-time=${dumpname}-time ${mode} ${flag} ${benchdir} 2>&1"

        echo "running ${cmd}"

        output=$(eval "${cmd}")
        result=$(echo "$output" | grep -E "Number of distinct graphs: [0-9]+ " | awk -F': ' '{print $2}' | awk '{print $1}')
        echo "result = ${result}"
        complete_execs=$(echo "$output" | grep -E "Number of distinct complete executions explored: [0-9]+" | awk -F': ' '{print $2}' | awk '{print $1}')
        wallclock=$(echo "$output" | grep -E "Total wall-clock time" | awk '{print $NF}' | tr -d 's')
        sec_per_graph=$(echo "scale=4; 1000 * $wallclock / $result" | bc)





    done
}



function run_selected_benchmarks {
    mkdir -p out/coverage

   
    local benchmarks_to_run=("$@")

    # echo "Benchmark,Mode,Coverage,Complete,Caption,Seconds,MsPerGraph" >$OUTPUT_FILE

    for name in "${benchmarks_to_run[@]}"; do

        run_benchmark "${name}" "${FUZZ_FLAGS}" "fuzz"

        run_benchmark "${name}" "${RANDOM_FLAGS}" "rand"

    done
}

N=$1
REPEAT=$2

# show usage if incorrect number of arguments
if [ "$#" -lt 2 ]; then
	echo "Usage: $0 <N> <repeat>"
	exit 1
fi

source ./flags.sh

# fuzz mode
FUZZ_FLAGS=$FUZZ_FLAGS_3phstar

#
selected=(
    "chase-lev"
    "dq"
    "dq-opt"
    "mpmc-queue"
    "ms-queue"
    "ms-queue-dynamic"
    "seqlock"
    "seqlock-atomic"
    "spinlock"
    "treiber-stack"
    "treiber-stack-dynamic"
    "stc"
    "linuxrwlocks"
    "mutex"
    "qu"
    "twalock"
    "stc-opt"
    "htable"
    "sharedptr"
    "buf-ring"
)

run_selected_benchmarks "${selected[@]}"

python3 figure2.py

# Compile PDF
pdflatex -output-directory=out/build main.tex

echo "PDF generated: out/build/main.pdf"

# Clean coverage CSV logs if present for downstream analysis
if [ -d "out/coverage" ]; then
    echo "Cleaning coverage logs under out/coverage ..."
    python3 clean_csv.py out/coverage/
fi