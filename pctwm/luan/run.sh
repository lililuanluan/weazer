#!/bin/bash

set -e

# if $BUILD == 1
if [[ "${BUILD:-0}" -eq 1 ]]; then
	cd ..
	make clean
	make -j"$(nproc)"
	cd -
fi

export LD_LIBRARY_PATH=..

# 参数校验要尽早进行，避免 set -e 因后续命令失败而提前退出
if [ $# -lt 5 ]; then
	echo "Usage: $0 <benchmark_name> <timeout> <repeat> <depth> <communication_events>"
	exit 1
fi

BENCHNAME=$1
EXE="bin/${BENCHNAME}"

# 将BENCHNAME后面括号中的数提取出来
N=$(echo "$BENCHNAME" | grep -oP '\(\K[0-9]+(?=\))')

# 将BENCHNAME除了括号部分的前缀提取出来
BENCH_PREFIX=$(echo "$BENCHNAME" | sed -E 's/\([0-9]+\)$//')

echo "Running benchmark: $BENCH_PREFIX with N=$N"

./build.sh $BENCH_PREFIX $N

VERSION=1
TIMEOUT=$2
REPEAT=$3
DEPTH=$4
COMMUNICATIONEVENTS=$5
HISTORY=0

# 取消set -e
set +e

ENVIRONMENT=" -x1 -p$VERSION -d$DEPTH -v3 -k$COMMUNICATIONEVENTS -y$HISTORY"

echo "C11TESTER = $ENVIRONMENT"
export C11TESTER=$ENVIRONMENT

declare -A HASH_SET=()
TOTAL_HASH_COUNT=0

output_file="out/${BENCHNAME}.csv"
echo "Benchmark,Method,Iter,Sec" >"$output_file"

function repetitive_execute() {
	TIMECOUNT=0
	ITERCOUNT=0
	# track max instrnum per repeat
	MAX_INSTRNUM=-1

	# 循环执行EXE，将其所需的真实时间累加，到TIMECOUNT上，当TIMECOUNT大于TIMEOUT时，停止循环
	while [ "$(echo "$TIMECOUNT < $TIMEOUT" | bc -l)" -eq 1 ]; do
		ITERCOUNT=$((ITERCOUNT + 1))

		rm -f C11FuzzerTmp*

		# 记录起止时间（纳秒），兼容不支持 %N 的系统
		START_NS=$(date +%s%N)
		[[ "$START_NS" == *N ]] && START_NS="$(($(date +%s) * 1000000000))"

		OUTPUT=$("$EXE" 2>&1)
		STATUS=$?

		# update MAX_INSTRNUM from lines like: "incrementing instrnum to 15"
		INSTR_VALS=$(echo "$OUTPUT" | sed -n 's/.*incrementing instrnum to \([0-9][0-9]*\).*/\1/p')
		if [[ -n "$INSTR_VALS" ]]; then
			for VAL in $INSTR_VALS; do
				if ((VAL > MAX_INSTRNUM)); then
					MAX_INSTRNUM=$VAL
				fi
			done
		fi

		# 提取并记录 HASH 数值（支持一次运行中多次出现，全部计入集合）
		HASHES=$(echo "$OUTPUT" | grep -oE 'HASH[[:space:]]+[0-9]+' | awk '{print $2}')
		if [[ -n "$HASHES" ]]; then
			for H in $HASHES; do
				HASH_SET["$H"]=1
				((TOTAL_HASH_COUNT++))
			done
			# 如果定义了环境变量VERBOSE，则打印HASH信息
			[[ "${VERBOSE:-0}" -eq 1 ]] &&
				echo "Captured HASH(es): $(echo $HASHES | xargs)"
		fi

		END_NS=$(date +%s%N)
		[[ "$END_NS" == *N ]] && END_NS="$(($(date +%s) * 1000000000))"

		# 计算耗时（秒）
		ELAPSED_NS=$((END_NS - START_NS))
		# 防止负数（极端情况下时钟回拨）
		if ((ELAPSED_NS < 0)); then ELAPSED_NS=0; fi
		REALTIME=$(echo "scale=6; $ELAPSED_NS/1000000000" | bc -l)

		# 信号崩溃检测：退出码 >= 128 表示被信号终止
		if ((STATUS >= 128)); then
			SIG=$((STATUS - 128))
			echo "Program terminated by signal $SIG"
			echo "output: $OUTPUT"
			echo "Stop running further iterations."
			break
		fi

		TIMECOUNT=$(echo "$TIMECOUNT + $REALTIME" | bc -l)

		# 如果定义了环境变量VERBOSE，则打印OUTPUT信息
		[[ "${VERBOSE:-0}" -eq 1 ]] &&
			echo "output: $OUTPUT"

		# 如果OUTPUT中含有"Abort."字样，跳出循环，打印所用时间和迭代次数
		break_flag=0
		if echo "$OUTPUT" | grep -q "Abort."; then
			echo "Benchmark aborted during iteration $ITERCOUNT. time: $TIMECOUNT seconds"
			break_flag=1
		fi

		# 如果定义了环境变量VERBOSE，则打印OUTPUT信息
		if [[ "${VERBOSE:-0}" -eq 1 ]]; then
			# 将"Change Priority"所在行打印出来
			echo "$OUTPUT" | grep "Change Priority"
			echo "Current run time: $REALTIME seconds, Total time: $TIMECOUNT seconds"
			echo "Iteration: $ITERCOUNT"
			echo "HASH unique/total: ${#HASH_SET[@]}/$ITERCOUNT"
			# Print max instrnum for this repeat in red (if found)
			if ((MAX_INSTRNUM >= 0)); then
				printf "\033[0;31mMax instrnum for this repeat: %d\033[0m\n" "$MAX_INSTRNUM"
			fi
		fi

		if [ $break_flag -eq 1 ]; then
			break
		fi
	done
	# 如果超时，则时间那一列写 '-'
	if [ "$(echo "$TIMECOUNT > $TIMEOUT" | bc -l)" -eq 1 ]; then
		TIMECOUNT='-'
	fi
	echo "$BENCHNAME,pctwm,$ITERCOUNT,$TIMECOUNT" >>"$output_file"

}

for i in $(seq 1 "$REPEAT"); do
	echo "=== Repeat $i/$REPEAT ==="
	repetitive_execute
done
