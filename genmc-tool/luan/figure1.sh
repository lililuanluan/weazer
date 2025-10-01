#!/bin/bash

REPEAT=$1
TIMEOUT=$2

function show_usage {
	echo "format: ./figure1.sh repeat timeout"
}

if [ $# -lt 2 ]; then
	echo "Error: Not enough arguments"
	show_usage
	exit 1
fi




benchmarks=(
	"dglm-queue(3)"
	"dglm-queue(4)"
	"dglm-queue(5)"
	"dglm-queue(6)"
	"dglm-queue(7)"
	"dglm-queue(8)"

	"dglm-queue2(3)"
	"dglm-queue2(4)"
	"dglm-queue2(5)"
	"dglm-queue2(6)"
	"dglm-queue2(7)"
	"dglm-queue2(8)"

	"dglm-queue3(3)"
	"dglm-queue3(4)"
	"dglm-queue3(5)"
	"dglm-queue3(6)"
	"dglm-queue3(7)"
	"dglm-queue3(8)"

	"ms-queue(3)"
	"ms-queue(4)"
	"ms-queue(5)"
	"ms-queue(6)"
	"ms-queue(7)"
	"ms-queue(8)"

	"ms-queue2(3)"
	"ms-queue2(4)"
	"ms-queue2(5)"
	"ms-queue2(6)"
	"ms-queue2(7)"
	"ms-queue2(8)"

	"ms-queue3(3)"
	"ms-queue3(4)"
	"ms-queue3(5)"
	"ms-queue3(6)"
	"ms-queue3(7)"
	"ms-queue3(8)"

	"ms-queue-opt(3)"
	"ms-queue-opt(4)"
	"ms-queue-opt(5)"
	"ms-queue-opt(6)"
	"ms-queue-opt(7)"
	"ms-queue-opt(8)"

	"ms-queue-opt2(3)"
	"ms-queue-opt2(4)"
	"ms-queue-opt2(5)"
	"ms-queue-opt2(6)"
	"ms-queue-opt2(7)"
	"ms-queue-opt2(8)"

	"ms-queue-opt3(3)"
	"ms-queue-opt3(4)"
	"ms-queue-opt3(5)"
	"ms-queue-opt3(6)"
	"ms-queue-opt3(7)"
	"ms-queue-opt3(8)"

	"treiber-stack(3)"
	"treiber-stack(4)"
	"treiber-stack(5)"
	"treiber-stack(6)"
	"treiber-stack(7)"
	"treiber-stack(8)"

	"treiber-stack2(3)"
	"treiber-stack2(4)"
	"treiber-stack2(5)"
	"treiber-stack2(6)"
	"treiber-stack2(7)"
	"treiber-stack2(8)"

	"treiber-stack3(3)"
	"treiber-stack3(4)"
	"treiber-stack3(5)"
	"treiber-stack3(6)"
	"treiber-stack3(7)"
	"treiber-stack3(8)"

	"optimized-lf-queue(3)"
	"optimized-lf-queue(4)"
	"optimized-lf-queue(5)"
	"optimized-lf-queue(6)"
	"optimized-lf-queue(7)"
	"optimized-lf-queue(8)"

	"optimized-lf-queue2(3)"
	"optimized-lf-queue2(4)"
	"optimized-lf-queue2(5)"
	"optimized-lf-queue2(6)"
	"optimized-lf-queue2(7)"
	"optimized-lf-queue2(8)"

	"optimized-lf-queue3(3)"
	"optimized-lf-queue3(4)"
	"optimized-lf-queue3(5)"
	"optimized-lf-queue3(6)"
	"optimized-lf-queue3(7)"
	"optimized-lf-queue3(8)"
)

OUTDIR="out/buggy/"
mkdir -p "$OUTDIR"


for bench in "${benchmarks[@]}"; do
	./4versions.sh $bench 0 $REPEAT $TIMEOUT rand 3phstar
done

mkdir -p out/buggy/plots/
mkdir -p out/buggy/coverage_plots/

python3 figure4.py

pdflatex -output-directory=out/build main.tex

# Clean coverage CSV logs if present for downstream analysis
if [ -d "out/coverage" ]; then
	echo "Cleaning coverage logs under out/coverage ..."
	python3 clean_csv.py out/coverage/
fi