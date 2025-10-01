#!/bin/bash
N=1000000000
source ./benchmarks.sh

TOOL=../genmc
TIMEOUT=$2

if [ $# -lt 2 ]; then
	echo "Error: Not enough arguments"
	echo "format: $0 repeat timeout"
	exit 1
fi

source ./flags.sh

mkdir -p out/shallow

OUTPUT_FILE="out/shallow/table3.csv"

echo "Benchmark,Method,Iter,Sec" >"$OUTPUT_FILE"

function test_benchmark {
	local benchname=$1
	local mode=$2
	local benchdir=${benchmarks_buggy[$benchname]}
	local flag=${flags_buggy[$benchname]}
	local repeat_count=$REPEAT
	local modename=$3
	local caption=$4



	i=0
	while [[ $i -lt $repeat_count ]]; do

		local dumpname="tmp"

		# cmd="${TOOL} ${mode}  ${flag} ${benchdir} 2>&1"
		cmd="timeout ${TIMEOUT}s ${TOOL} ${mode} --dump-coverage=\"${dumpname}\" ${flag} ${benchdir} 2>&1"

		echo "running ${cmd}"
		output=$(eval $cmd)

		result=$(echo "$output" | grep -E "Number of distinct graphs" | awk -F'/' '{print $2}' | awk '{print $1}') # get iteration
		cover=$(echo "$output" | grep -E "Number of distinct graphs" | awk -F'=' '{print $2}' | awk '{print $1}' | tr -d '()%')

		sec=$(echo "$output" | grep -oP '(?<=Total wall-clock time: )\d+\.\d+(?=s)')
		sec_per_graph=$(echo "scale=4; 1000 * $sec / $result" | bc)
		# get coverage percentage

		((i++))
		# if [[ $i == 1 ]]; then
		# 	echo "running  $cmd"
		# fi

		complete_execs=$(echo "$output" | grep -E "Number of distinct complete executions explored: [0-9]+" | awk -F': ' '{print $2}' | awk '{print $1}')

		rndCnt=$(echo "$output" | awk '/random exploration:/ {print $NF}')
		mutCnt=$(echo "$output" | awk '/mutation count:/ {print $NF}')
		totalCnt=$(echo "$output" | awk '/total work:/ {print $NF}')
		if [ "$totalCnt" -eq 0 ]; then
			echo "Total count is zero. Output:"
			echo "$output"
		fi
		rndRatio=$(echo "scale=4; $rndCnt / $totalCnt" | bc)
		mutRatio=$(echo "scale=4; $mutCnt / $totalCnt" | bc)
		echo "${benchname},rand = ${rndRatio}, mut = ${mutRatio}, iter = ${result}, coverage = ${cover}%, method = ${caption}, complete = ${complete_execs}, seconds = ${sec}"

		if [ -z "$result" ] || [ -z "$sec" ]; then
			stdbuf -o0 echo -e "\n\e[31mparse output failed\e[0m"
			echo "run ${cmd}"
			echo -e "${benchname},${caption},-,-" >>"$OUTPUT_FILE"
			# break
		else
			echo "${benchname},${caption},${result},${sec}" >>$OUTPUT_FILE
		fi
	done
}

shallow_benchmarks=(
	"ms-queue-write-bug(3)"
	"ms-queue-write-bug(4)"
	"ms-queue-write-bug(5)"
	"ms-queue-write-bug(6)"
	"ms-queue-write-bug(7)"
	"ms-queue-write-bug(8)"

	"ms-queue-xchg-bug(3)"
	"ms-queue-xchg-bug(4)"
	"ms-queue-xchg-bug(5)"
	"ms-queue-xchg-bug(6)"
	"ms-queue-xchg-bug(7)"
	"ms-queue-xchg-bug(8)"

	"ms-queue2-write-bug(3)"
	"ms-queue2-write-bug(4)"
	"ms-queue2-write-bug(5)"
	"ms-queue2-write-bug(6)"
	"ms-queue2-write-bug(7)"
	"ms-queue2-write-bug(8)"

	"ms-queue2-xchg-bug(3)"
	"ms-queue2-xchg-bug(4)"
	"ms-queue2-xchg-bug(5)"
	"ms-queue2-xchg-bug(6)"
	"ms-queue2-xchg-bug(7)"
	"ms-queue2-xchg-bug(8)"

	"ms-queue3-write-bug(3)"
	"ms-queue3-write-bug(4)"
	"ms-queue3-write-bug(5)"
	"ms-queue3-write-bug(6)"
	"ms-queue3-write-bug(7)"
	"ms-queue3-write-bug(8)"

	"ms-queue3-xchg-bug(3)"
	"ms-queue3-xchg-bug(4)"
	"ms-queue3-xchg-bug(5)"
	"ms-queue3-xchg-bug(6)"
	"ms-queue3-xchg-bug(7)"
	"ms-queue3-xchg-bug(8)"
)

REPEAT=$1

function run_selected_benchmarks {
	local benchmarks_to_run=("$@")

	# verify
	for name in "${benchmarks_to_run[@]}"; do
		test_benchmark "${name}" "${RANDOM_FLAGS}" "rand" "Random"
		test_benchmark "${name}" "${FUZZ_FLAGS_3phstar}" "fuzz" "3phstar"
		test_benchmark "${name}" "${VERIF_FLAGS}" "GenMC" "GenMC"
	done
}

run_selected_benchmarks "${shallow_benchmarks[@]}"

python3 table3.py > out/shallow/table3.tex

# Clean coverage CSV logs if present for downstream analysis
if [ -d "out/coverage" ]; then
	echo "Cleaning coverage logs under out/coverage ..."
	python3 clean_csv.py out/coverage/
fi