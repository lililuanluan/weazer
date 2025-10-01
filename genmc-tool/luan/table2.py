import pandas as pd
import numpy as np

from table1 import get_data, get_df


benchnames = [
    "CS-account",
    "CS-bluetooth_driver",
    "CS-circular_buffer",
    "CS-lazy01",
    "CS-queue",
    "CS-reorder_3",
    "CS-reorder_4",
    "CS-reorder_5",
    "CS-reorder_10",
    "CS-reorder_20",
    "CS-reorder_50",
    "CS-reorder_100",
    "CS-stack",
    "CS-token_ring",
    "CS-twostage",
    "CS-wronglock",
    "CVE-2009-3547",
    "CVE-2013-1792",
    "CVE-2015-7550",
    "CVE-2016-9806",
    "CVE-2017-15265",
]


def print_table():
    df = get_df("out/rff/")
    print(r"\begin{table}[!t]")
    print(r"\centering")
    print(r"\caption{}")
    print(r"\label{tab:rff-bench-time}")
    print(r"\scriptsize")
    print(r"\begin{tabular}{lcccccc}")
    print(r"\hline")

    print(
        r"\multirow{2}{*}{\textbf{Benchmark}}  &  \multicolumn{2}{c}{\bf\genmc}    & \multicolumn{2}{c}{\bf\random}    & \multicolumn{2}{c}{\textbf{$\algname$}}    \\"
        + "\n"
    )
    print(r"\cline{2-7}" + "\n")
    print(
        r"&   Sec    & \%     & Sec                      & \%   & Sec                     & \%         \\ \hline"
        + "\n"
    )

    for bench in benchnames:
        get_data(benchname=bench, df=df)

    print(r"\hline" + "\n" + r"\end{tabular}" + "\n" + r"\end{table}")


if __name__ == "__main__":
    print_table()
