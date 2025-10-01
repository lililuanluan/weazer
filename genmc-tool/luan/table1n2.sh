#!/bin/bash
 
# Check if we have at least 5 arguments (benchmark, output_dir, repeat, timeout, and at least one mode)
if [ $# -lt 5 ]; then
	echo "Error: Not enough arguments"
	show_usage
fi

# Parse command line arguments
BENCHMARK_NAME=$1
OUTPUT_DIR=$2
REPEAT=$3
TIMEOUT=$4
shift 4 # Remove first four arguments

# Get the modes to run
MODES_TO_RUN=("$@")

# Validate repeat and timeout are numbers
if ! [[ "$REPEAT" =~ ^[0-9]+$ ]]; then
	echo "Error: repeat must be a positive integer"
	show_usage
fi

if ! [[ "$TIMEOUT" =~ ^[0-9]+$ ]]; then
	echo "Error: timeout must be a positive integer"
	show_usage
fi

# Validate modes
valid_modes=("verify" "rand" "3phstar")
for mode in "${MODES_TO_RUN[@]}"; do
	if [[ ! " ${valid_modes[@]} " =~ " ${mode} " ]]; then
		echo "Error: Invalid mode '$mode'. Valid modes are: ${valid_modes[*]}"
		show_usage
	fi
done



N=1000000000
source ./benchmarks.sh

TOOL=../genmc
source ./flags.sh

# Create output directory
mkdir -p "${OUTPUT_DIR}/"

# set -x

function run_benchmark {
	local benchname=$1
	local mode=$2
	local caption=$3

	# Create output file based on benchmark name
	OUTPUT_FILE="${OUTPUT_DIR}/${BENCHMARK_NAME}-${caption}.csv"

	echo "Benchmark,Method,Iter,Sec" >"$OUTPUT_FILE"

	# Determine benchmark directory and flags
	if [[ -v benchmarks_buggy[$benchname] ]]; then
		local benchdir=${benchmarks_buggy[$benchname]}
		local flag=${flags_buggy[$benchname]}
	else
		local benchdir=${benchmarks_correct[$benchname]}
		local flag=${flags_correct[$benchname]}
	fi

	# Use the global REPEAT value for all modes
	local repeat_count=$REPEAT

	# If enabled, force verification (GenMC) mode to run only once
	if [[ "$mode" == "$VERIF_FLAGS" && "$SKIP_REPEAT_GENMC" == "1" ]]; then
		repeat_count=1
	fi

	# Run the benchmark for the specified number of iterations
	for ((i = 0; i < repeat_count; i++)); do
		local dumpname="tmp"

		# Build command based on mode
		if [[ "$mode" == "$VERIF_FLAGS" ]]; then
			cmd="timeout ${TIMEOUT}s ${TOOL} ${mode} ${flag} ${benchdir} 2>&1"
		else

			cmd="timeout ${TIMEOUT}s ${TOOL} ${mode}   ${flag} ${benchdir} 2>&1"
		fi

		echo "Running ${cmd}"
		output=$(eval "${cmd}")

		# Parse common output metrics
		result=$(echo "$output" | grep -E "Number of distinct graphs" | awk -F'/' '{print $2}' | awk '{print $1}')
		sec=$(echo "$output" | grep -oP '(?<=Total wall-clock time: )\d+\.\d+(?=s)')
		complete_execs=$(echo "$output" | grep -E "Number of distinct complete executions explored: [0-9]+" | awk -F': ' '{print $2}' | awk '{print $1}')

		# Parse mode-specific metrics
		if [[ "$mode" != "$VERIF_FLAGS" ]]; then
			# For random/fuzz modes, parse coverage and mutation statistics
			cover=$(echo "$output" | grep -E "Number of distinct graphs" | awk -F'=' '{print $2}' | awk '{print $1}' | tr -d '()%')
			sec_per_graph=$(echo "scale=4; 1000 * $sec / $result" | bc)

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
		else
			# For verification mode, check completion status
			if echo "$output" | grep -q "Verification complete."; then
				verification_status="x"
			else
				verification_status="hit bug"
			fi
			echo -e "${benchname}\t\t${result} (verification: ${verification_status})"
		fi

		# Handle parsing failures and write results
		if [ -z "$result" ] || [ -z "$sec" ]; then
			stdbuf -o0 echo -e "\n\e[31mparse output failed\e[0m"
			echo -e "${benchname},${caption},-,-" >>"$OUTPUT_FILE"

		else
			echo "${benchname},${caption},${result},${sec}" >>$OUTPUT_FILE
		fi
	done
}


function run_selected_benchmarks {
	local benchmarks_to_run=("$@")

	for name in "${benchmarks_to_run[@]}"; do
		echo "=== Running benchmark: ${name} ==="

		# Commented out skip logic - continue testing all modes even after failures
		# local skip_remaining=0

		# Run each requested mode
		for mode in "${MODES_TO_RUN[@]}"; do
			# Commented out skip logic - continue testing even after failures
			# if [ $skip_remaining -eq 1 ]; then
			#	echo "Skipping $mode mode for ${name} due to previous failure"
			#	continue
			# fi

			echo "--- ${mode} mode ---"

			case "$mode" in
			"verify")
				run_benchmark "${name}" "${VERIF_FLAGS}" "GenMC"

				;;
			"rand")
				run_benchmark "${name}" "${RANDOM_FLAGS}" "Random"
				;;
			"3phstar")
				run_benchmark "${name}" "${FUZZ_FLAGS_3phstar}" "3phstar"
				;;
			*)
				echo "Error: Unknown mode '$mode'"
				;;
			esac
		done

		echo "=== Completed benchmark: ${name} ==="
		echo
	done
}

# Run the single benchmark specified by command line argument
echo "=== Running benchmark: ${BENCHMARK_NAME} ==="

# Run each requested mode
for mode in "${MODES_TO_RUN[@]}"; do
	echo "--- ${mode} mode ---"

	case "$mode" in
	"verify")
		run_benchmark "${BENCHMARK_NAME}" "${VERIF_FLAGS}" "GenMC"
		;;
	"rand")
		run_benchmark "${BENCHMARK_NAME}" "${RANDOM_FLAGS}" "Random"
		;;
	"3phstar")
		run_benchmark "${BENCHMARK_NAME}" "${FUZZ_FLAGS_3phstar}" "3phstar"
		;;
	*)
		echo "Error: Unknown mode '$mode'"
		;;
	esac
done

echo "=== Completed benchmark: ${BENCHMARK_NAME} ==="
