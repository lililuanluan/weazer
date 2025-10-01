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
synthetic_benchmarks=(

	"long-assert(5)"
	"long-assert(6)"
	"long-assert(7)"
	"long-assert(2)"
	"long-assert(3)"
	"long-assert(4)"

	"n1-val(10)"
	"n1-val(50)"
	"n1-val(100)"
	"n1-val(200)"
	"n1-val(500)"
	"n1-val(1000)"

	"mp(5)"
	"mp(6)"
	"mp(7)"
	"mp(8)"
	"mp(9)"
	"mp(10)"

)

# Create output directory
OUTDIR="out/synthetic/"
mkdir -p "$OUTDIR"



# Run each benchmark using table1n2.sh
for benchmark in "${synthetic_benchmarks[@]}"; do
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

# Create output build directory
mkdir -p out/build

# Generate LaTeX table
python3 table1.py >out/synthetic/table.tex

# Compile PDF
pdflatex -output-directory=out/build main.tex

echo "PDF generated: out/build/main.pdf"

# Clean coverage CSV logs if present for downstream analysis
# Usage: python3 clean_csv.py <folder>
if [ -d "out/coverage" ]; then
	echo "Cleaning coverage logs under out/coverage ..."
	python3 clean_csv.py out/coverage/
fi
