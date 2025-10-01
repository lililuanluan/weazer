import pandas as pd
import numpy as np

from table1 import get_data

benchnames = [
    "ms-queue-write-bug(3)",
    "ms-queue-write-bug(4)",
    "ms-queue-write-bug(5)",
    "ms-queue-write-bug(6)",
    "ms-queue-write-bug(7)",
    "ms-queue-write-bug(8)",
    "ms-queue-xchg-bug(3)",
    "ms-queue-xchg-bug(4)",
    "ms-queue-xchg-bug(5)",
    "ms-queue-xchg-bug(6)",
    "ms-queue-xchg-bug(7)",
    "ms-queue-xchg-bug(8)",
    "ms-queue2-write-bug(3)",
    "ms-queue2-write-bug(4)",
    "ms-queue2-write-bug(5)",
    "ms-queue2-write-bug(6)",
    "ms-queue2-write-bug(7)",
    "ms-queue2-write-bug(8)",
    "ms-queue2-xchg-bug(3)",
    "ms-queue2-xchg-bug(4)",
    "ms-queue2-xchg-bug(5)",
    "ms-queue2-xchg-bug(6)",
    "ms-queue2-xchg-bug(7)",
    "ms-queue2-xchg-bug(8)",
    "ms-queue3-write-bug(3)",
    "ms-queue3-write-bug(4)",
    "ms-queue3-write-bug(5)",
    "ms-queue3-write-bug(6)",
    "ms-queue3-write-bug(7)",
    "ms-queue3-write-bug(8)",
    "ms-queue3-xchg-bug(3)",
    "ms-queue3-xchg-bug(4)",
    "ms-queue3-xchg-bug(5)",
    "ms-queue3-xchg-bug(6)",
    "ms-queue3-xchg-bug(7)",
    "ms-queue3-xchg-bug(8)",
]




def print_table():
    print(r"\begin{table}[ht]")
    print(r"\centering")
    print(r"\caption{TODO}")
    print(r"\label{tab:tab:ds-mem-bugs-time}")
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

    df = pd.read_csv("out/shallow/table3.csv")
    

    for bench in [name for name in benchnames if "ms-queue-" in name]:
        get_data(benchname=bench, df=df, Entry="Sec")
    
    print(r"\midrule" + "\n")
    
    for bench in [name for name in benchnames if "ms-queue2" in name]:
        get_data(benchname=bench, df=df, Entry="Sec")
        
    print(r"\midrule" + "\n")
    
    for bench in [name for name in benchnames if "ms-queue3" in name]:
        get_data(benchname=bench, df=df, Entry="Sec")

    print(r"\hline" + "\n" + r"\end{tabular}" + "\n" + r"\end{table}")

if __name__ == "__main__":
    print_table()
