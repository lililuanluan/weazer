#!/bin/bash

set -e

if [ $# -lt 3 ]; then
	echo "Error: Not enough arguments"
fi

REPEAT=$1
TIMEOUT=$2
shift 2

MODES_TO_RUN=(
	"rand"
	"3phstar"
	"verify"
)

if ! [[ "$REPEAT" =~ ^[0-9]+$ ]]; then
	echo "Error: repeat must be a positive integer"
	show_usage
fi

if ! [[ "$TIMEOUT" =~ ^[0-9]+$ ]]; then
	echo "Error: timeout must be a positive integer"
	show_usage
fi

valid_modes=("verify" "rand" "3phstar")
for mode in "${MODES_TO_RUN[@]}"; do
	if [[ ! " ${valid_modes[@]} " =~ " ${mode} " ]]; then
		echo "Error: Invalid mode '$mode'. Valid modes are: ${valid_modes[*]}"
		show_usage
	fi
done

echo "Running all benchmarks with parameters:"
echo "  Repeat: $REPEAT"
echo "  Timeout: ${TIMEOUT}s"
echo "  Modes: ${MODES_TO_RUN[*]}"
echo ""

# Define synthetic benchmarks list
rff_benchmarks=(

	"CS-account"
	"CS-bluetooth_driver"
	"CS-circular_buffer"
	"CS-lazy01"
	"CS-queue"
	"CS-reorder_10"
	"CS-reorder_20"
	"CS-reorder_50"
	"CS-reorder_100"
	"CS-reorder_3"
	"CS-reorder_4"
	"CS-reorder_5"
	"CS-stack"
	"CS-token_ring"
	"CS-twostage"
	"CS-wronglock"
	"CVE-2009-3547"
	"CVE-2013-1792"
	"CVE-2015-7550"
	"CVE-2016-9806"
	"CVE-2017-15265"

)

# Create output directory
OUTDIR="out/rff/"
mkdir -p "$OUTDIR"



# Run each benchmark using table1n2.sh
for benchmark in "${rff_benchmarks[@]}"; do
	echo "======================"
	echo "Running benchmark: ${benchmark}"
	echo "======================"

	# Call table1n2.sh with the benchmark name, output directory, and other parameters
	./table1n2.sh "${benchmark}" "$OUTDIR" "${REPEAT}" "${TIMEOUT}" "${MODES_TO_RUN[@]}"

	echo ""
	echo "Completed: ${benchmark}"
	echo ""
done

echo "======================"
echo "All benchmarks completed!"
echo "======================"

mkdir -p out/build

python3 table2.py >out/rff/table.tex

pdflatex -output-directory=out/build main.tex

echo "PDF generated: out/build/main.pdf"

# Clean coverage CSV logs if present for downstream analysis
if [ -d "out/coverage" ]; then
	echo "Cleaning coverage logs under out/coverage ..."
	python3 clean_csv.py out/coverage/
fi