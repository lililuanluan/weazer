import pandas as pd
import numpy as np
import os
import glob

benchnames = [
    "long-assert(2)",
    "long-assert(3)",
    "long-assert(4)",
    "long-assert(5)",
    "long-assert(6)",
    "long-assert(7)",
    "mp(5)",
    "mp(6)",
    "mp(7)",
    "mp(8)",
    "mp(9)",
    "mp(10)",
    "n1-val(10)",
    "n1-val(50)",
    "n1-val(100)",
    "n1-val(200)",
    "n1-val(500)",
    "n1-val(1000)",
]


def get_data(benchname, df, Entry="Sec", skip_percent=False):
    print(f"\nProcessing benchmark: {benchname}", file=os.sys.stderr)
    line = benchname.replace("_", r"\_") + r" & "
    for method in ["GenMC", "Random", "3phstar"]:
        df_synthetic = df[
            (df["Benchmark"] == benchname) & (df["Method"] == method)
        ].copy()

        df_synthetic.loc[:, Entry] = pd.to_numeric(df_synthetic[Entry], errors="coerce")

        if df_synthetic.empty:
            print(
                f"No data found for benchmark: {benchname}, method: {method}",
                file=os.sys.stderr,
            )
            avg_var = r"\bugnotfound"
            percentage = r"0.0\%"
        else:
            valid_data = df_synthetic[Entry].dropna()
            valid_data = valid_data[valid_data >= 0]  # 移除负值（如果有的话）

            total_runs = len(df_synthetic)
            successful_runs = len(valid_data)

            if successful_runs == 0:
                # 所有运行都超时
                avg_var = r"\clock"
                percentage = r"0.0\%"
            else:
                # 计算均值和标准差
                mean = valid_data.mean()
                std_dev = valid_data.std()
                success_rate = (successful_runs / total_runs) * 100

                if successful_runs == 1:
                    avg_var = f"{mean:.2f}"
                else:
                    avg_var = f"{mean:.2f}" + r" $\pm$ " + f"{std_dev:.2f}"
                percentage = f"{success_rate:.1f}\\%"

        line += avg_var + " & "
        if not skip_percent:
            line += percentage + " & "
    line = line.rstrip(" & ")

    line += r" \\"
    print(line)


def get_df(dir):
    # Create output directory if it doesn't exist
    os.makedirs(dir, exist_ok=True)

    # Combine all CSV files in the out/synthetic directory
    all_files = glob.glob(f"{dir}*.csv")
    print(f"all_files: {all_files}", file=os.sys.stderr)
    df_list = []
    for file in all_files:
        if "summary" not in file:  # Skip summary.csv if it exists
            df = pd.read_csv(file)
            df_list.append(df)

    if not df_list:
        print("No CSV files found in out/synthetic/")
        return

    df = pd.concat(df_list, ignore_index=True)
    return df


def print_table():
    df = get_df("out/synthetic/")

    print(r"\begin{table}[!t]")
    print(r"\centering")
    print(r"\caption{}")
    print(r"\label{tab:syntatic-time}")
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

    for bench in [name for name in benchnames if "long-assert" in name]:
        get_data(benchname=bench, df=df)

    print(r"\midrule" + "\n")

    # mp(..)
    for bench in [name for name in benchnames if "mp" in name]:
        get_data(benchname=bench, df=df)
    print(r"\midrule" + "\n")

    for bench in [name for name in benchnames if "n1-val" in name]:
        get_data(benchname=bench, df=df)

    # mp(..)
    print(r"\hline" + "\n" + r"\end{tabular}" + "\n" + r"\end{table}")


if __name__ == "__main__":
    print_table()
