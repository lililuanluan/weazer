#!/bin/bash

repeat=30
timeout=1800 # 30 min

./run.sh "n1-val(10)" $timeout $repeat 2 22
./run.sh "n1-val(50)" $timeout $repeat 2 102
./run.sh "n1-val(100)" $timeout $repeat 2 202
./run.sh "n1-val(200)" $timeout $repeat 2 402
./run.sh "n1-val(500)" $timeout $repeat 2 1002
./run.sh "n1-val(1000)" $timeout $repeat 2 2002