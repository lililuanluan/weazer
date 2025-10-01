#!/bin/bash
N=1000000000
source ./benchmarks.sh

TOOL=../genmc

mkdir -p out/buggy/coverage/

source ./flags.sh

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

benchmarkset=$1

function show_usage {
	echo "format: ./4versions bench start end [modes...]"
}

startfrom=$2
if ! [[ "$startfrom" =~ ^[0-9]+$ ]]; then
	show_usage
	exit 1
fi

selected_benchmarks=()

for bench in "${benchmarks[@]}"; do
	if [[ "$bench" == "$benchmarkset"* ]]; then
		selected_benchmarks+=("$bench")
	fi
done

printf '%s\n' "${selected_benchmarks[@]}"
if [ ${#selected_benchmarks[@]} -eq 0 ]; then
	echo "error, no benchmark in benchset: '$benchmarkset'"
	exit 1
fi

mkdir -p out/buggy/data

function test_benchmark {
	local benchname=$1
	local mode=$2
	local benchdir=${benchmarks_buggy[$benchname]}
	local flag=${flags_buggy[$benchname]}
	local repeat_count=$REPEAT
	local modename=$3
	local caption=$4

	if [[ "$mode" == "$VERIF_FLAGS" ]]; then
		repeat_count=1
	fi

	OUTPUT_FILE="out/buggy/data/4versions-$benchmarkset-${caption}.csv"
	if [ ! -f "$OUTPUT_FILE" ]; then
		echo "Benchmark,Method,Iter,Sec" >"$OUTPUT_FILE"
	fi

	i=$((startfrom + 1))
	repeat_count=$((repeat_count + 1))
	while [[ $i -lt $repeat_count ]]; do

		local dumpname="out/buggy/coverage/${benchname}-${caption}-${i}"

		# cmd="${TOOL} ${mode}  ${flag} ${benchdir} 2>&1"
		if [ "$NODUMP" == "1" ]; then
			cmd="timeout ${TIMEOUT}s ${TOOL} ${mode} ${flag} ${benchdir} 2>&1"
		else
			cmd="timeout ${TIMEOUT}s ${TOOL} ${mode} --dump-coverage=\"${dumpname}\" ${flag} ${benchdir} 2>&1"
		fi

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
		echo "${benchname},rand = ${rndRatio}, mut = ${mutRatio}, iter = ${result}, coverage = ${cover}%, method = ${caption}, complete = ${complete_execs}"

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

REPEAT=$3
TIMEOUT=$4 
printf "benchmarking bug detection speed, N = ${N}, repeat ${REPEAT} times, time budget ${TIMEOUT}\n"

shift
shift
shift
shift
methods=("$@")

function run_selected_benchmarks {
	local benchmarks_to_run=("$@")

	if [ ${#methods[@]} -eq 0 ]; then
		local methods_to_run=("rand" "3phstar" "star" "no")
	else
		local methods_to_run=("${methods[@]}")
	fi

	for method in "${methods_to_run[@]}"; do
		case "$method" in
		"rand" | "3phstar" | "star" | "no")
			;;
		*)
			show_usage
			exit 1
			;;
		esac
	done

	for name in "${benchmarks_to_run[@]}"; do
		echo ">>>> $name"

		for method_to_run in "${methods_to_run[@]}"; do
			case "$method_to_run" in
			"rand")
				test_benchmark "${name}" "${RANDOM_FLAGS}" "rand" "Random"
				;;
			"3phstar")
				test_benchmark "${name}" "${FUZZ_FLAGS_3phstar}" "fuzz" "3phstar"
				;;
			"star")
				test_benchmark "${name}" "${FUZZ_FLAGS_star}" "fuzz" "star"
				;;
			"no")
				test_benchmark "${name}" "${FUZZ_FLAGS_no}" "fuzz" "no"
				;;
			esac
		done
	done
}

run_selected_benchmarks "${selected_benchmarks[@]}"
