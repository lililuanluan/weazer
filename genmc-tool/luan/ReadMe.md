# Read me


# table1

Run `./table1.sh <repeat> <timeout>` to reproduce Table 1. The argument repeat is the number of repeated runs for each benchmark. Timeout stands for the time budget for each run, in seconds. 

For GenMC, since it's deterministic, the time to find the bug tends not to variate too much. If you don't want to run it repeatedly, you can `SKIP_REPEAT_GENMC=1   ./table1.sh <repeat> <timeout>`.

For example, if you want to see a quick result and want to run each benchmark with each method 5 times, each run with a time limit 30 seconds, and you don't want to run genmc repeatedly, you can run:
```bash
SKIP_REPEAT_GENMC=1  ./table1.sh 5 30
```

To reproduce Table 1 in the paper, you can run:
```bash
./table1.sh 30 1800
```

# table2

Similar to table1, run:

```bash
./table2.sh 30 1800
```

# figure 1

Run
```bash
./figure1.sh 30 1800
```

to reproduce the results in Figure 1. The error-bar plots are generated under `out/buggy/plots/`.

# figure 2

Run `./figure2.sh <N> <repeat>` to get coverage plots for safe benchmarks, where N stands for max iteration. For a quick result, you could run:

```bash
./figure2 1000 5
```

To reproduce Figure 2, run:

```bash
./figure2.sh 10000 30
``` 

# figure 4

after running  `figure1.sh`, run:

```bash
python3 figure4.py
```

to get the coverage plots.
