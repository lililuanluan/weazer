import pandas as pd
import numpy as np
import os
import re
import glob
import sys
import matplotlib.pyplot as plt
import argparse
from scipy.stats import mannwhitneyu
from lifelines.statistics import multivariate_logrank_test
from lifelines import CoxPHFitter

try:  # buggy data helpers (not needed for synthetic path)
    from buggy import load_buggy_data as _load_buggy_data, MAXN as _BUGGY_MAXN  # type: ignore
except Exception:
    _load_buggy_data = None
    _BUGGY_MAXN = None

DEFAULT_MAXN = 10**9



def stratified_mann_whitney_test(
    df, group_col="Caption", value_col="Coverage", strata_col="Benchmark"
):
    """
    Perform a stratified Mann-Whitney U test considering different strata (benchmarks).

    Args:
        df (pd.DataFrame): Input dataframe with group, value, and strata columns.
        group_col (str): Column name for grouping (e.g., 'Caption').
        value_col (str): Column name for values to compare (e.g., 'Coverage').
        strata_col (str): Column name for strata (e.g., 'Benchmark').

    Returns:
        float: Weighted U-statistic.
        float: Combined p-value.
    """
    strata = df[strata_col].unique()
    u_values = []
    p_values = []
    weights = []

    for stratum in strata:
        # Filter data for the current stratum
        stratum_data = df[df[strata_col] == stratum]
        groups = stratum_data[group_col].unique()

        # Ensure we have exactly two groups to compare
        if len(groups) != 2:
            continue

        group1 = stratum_data[stratum_data[group_col] == groups[0]][value_col]
        group2 = stratum_data[stratum_data[group_col] == groups[1]][value_col]

        # Perform Mann-Whitney U Test for the stratum
        u_stat, p_value = mannwhitneyu(group1, group2, alternative="two-sided")

        # Store results and weight
        u_values.append(u_stat)
        p_values.append(p_value)
        weights.append(
            len(group1) + len(group2)
        )  # Weight by total sample size in the stratum

    # Weighted U-statistic
    weighted_u = np.average(u_values, weights=weights)

    # Combine p-values using Fisher's method
    from scipy.stats import combine_pvalues

    combined_p = combine_pvalues(p_values, method="fisher")[1]

    return weighted_u, combined_p


def perform_global_mann_whitney_test(df_safe):
    # 执行分层 Mann-Whitney 测试
    u_stat, p_value = stratified_mann_whitney_test(df_safe)
    
    # 打印结果
    print("Stratified Mann-Whitney U Test:")
    print(f"  Weighted U-statistic: {u_stat:.2f}")
    print(f"  Combined P-value: {p_value:.4e}")
    
    # 判断显著性水平
    if p_value < 0.01:
        print("  Result: Highly significant difference (**) across strata")
    elif p_value < 0.05:
        print("  Result: Significant difference (*) across strata")
    else:
        print("  Result: No significant difference (-)")


def _infer_caption_from_filename(filename: str) -> str:
    # map "*-rand-*"->"Random", "*-fuzz-*"->"3phstar" (based on coverage directory naming)
    name = os.path.basename(filename)
    if "-rand-" in name:
        return "Random"
    if "-fuzz-" in name:
        return "3phstar"
    return "Unknown"


def _infer_benchmark_from_filename(filename: str) -> str:
    # remove trailing method and index segments
    base = os.path.basename(filename)
    parts = base.split('-')
    # drop last segment (index) and method segment
    if len(parts) >= 3:
        bench = '-'.join(parts[:-2])
    else:
        bench = base
    return bench


def load_coverage_benchmark_set(data_dir: str):
    """
    Load coverage run files under data_dir and compute per-run coverage rate = last Cover / last SecElapsed.
    Returns DataFrame columns: Benchmark, Caption, Coverage (rate)
    """
    records = []
    for root, _, files in os.walk(data_dir):
        for fn in files:
            path = os.path.join(root, fn)
            try:
                if os.path.getsize(path) == 0:
                    continue
                last_valid = None
                with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                    for line in f:
                        line = line.strip()
                        if not line or line.startswith('Iter,Cover,SecElapsed'):
                            continue
                        if line.count(',') != 2:
                            continue
                        a, b, c = line.split(',')
                        try:
                            it = int(a)
                            cv = int(b)
                            se = float(c)
                            if se > 0:
                                last_valid = (it, cv, se)
                        except ValueError:
                            continue
                if last_valid is None:
                    continue
                _, cover, sec = last_valid
                rate = cover / sec if sec > 0 else 0.0
                benchmark = _infer_benchmark_from_filename(path)
                caption = _infer_caption_from_filename(path)
                records.append({
                    'Benchmark': benchmark,
                    'Caption': caption,
                    'Coverage': rate,
                    'Source': path,
                })
            except Exception:
                continue
    return pd.DataFrame.from_records(records)


def coverage_mann_whitney_analysis(data_dir: str, m1: str = 'Random', m2: str = '3phstar'):
    df = load_coverage_benchmark_set(data_dir)
    if df.empty:
        print(f"No coverage data found under {data_dir}")
        return
    df = df[df['Caption'].isin([m1, m2])].copy()
    if df.empty:
        print(f"No records for {m1} and {m2} under {data_dir}")
        return
    # 描述性统计
    medians = df.groupby(['Benchmark','Caption'])['Coverage'].median().unstack()
    print("="*80)
    print(f"Coverage benchmark set: {data_dir}")
    print(f"Compare {m1} vs {m2} on coverage rate = Cover/SecElapsed (last line per run)")
    print("Medians by benchmark:")
    with pd.option_context('display.max_rows', None, 'display.width', 120):
        print(medians.fillna('-'))

    # 分层 Mann-Whitney U（按 Benchmark 分层）
    u_stat, p_value = stratified_mann_whitney_test(df, group_col='Caption', value_col='Coverage', strata_col='Benchmark')
    print("\nStratified Mann-Whitney U Test (by Benchmark):")
    print(f"  Weighted U: {u_stat:.3f}")
    print(f"  Combined P: {p_value:.6g}")
    # 方向性：比较两组总体中位数，可简单用全体中位数比较提示胜者
    overall = df.groupby('Caption')['Coverage'].median()
    if overall.get(m2, 0) > overall.get(m1, 0):
        winner = m2
    elif overall.get(m2, 0) < overall.get(m1, 0):
        winner = m1
    else:
        winner = 'Tie'
    print(f"  Winner (by overall median): {winner}")


def cox_regression(df, version1, version2):

    # 过滤出指定版本的数据
    filtered_df = df[df["Caption"].isin([version1, version2])].copy()

    # 为 Group 列赋值：1 表示 version1，2 表示 version2
    filtered_df["Group"] = filtered_df["Caption"].map({version1: 1, version2: 2})

    # 添加 Event 列：1 表示事件发生，0 表示事件未发生（删失）
    max_n = _BUGGY_MAXN if _BUGGY_MAXN is not None else DEFAULT_MAXN
    filtered_df["Event"] = filtered_df["Iter"].apply(lambda x: 1 if x < max_n else 0)

    # 准备 Cox 模型数据
    cph_data = filtered_df[["Iter", "Group", "Benchmark", "Event"]]

    # 拟合 Cox 模型
    cph = CoxPHFitter()
    cph.fit(cph_data, duration_col="Iter", event_col="Event", strata="Benchmark")

    # 打印模型结果
    # cph.print_summary()  # 显示回归系数和 p 值
    z_value = cph.summary.loc["Group", "z"]
    p_value = cph.summary.loc["Group", "p"]

    print(f"Z/P - values for v1 = {version1} vs v2 = {version2}: {z_value} & {p_value} \\\\")
    return z_value, p_value


def mann_whitney_test():
    df_safe = pd.read_csv("out/coverage.csv")
    perform_global_mann_whitney_test(df_safe)


    


def load_synthetic_data(data_dir="out/synthetic/"):
    """
    加载synthetic目录下的所有CSV文件数据
    
    Returns:
        pd.DataFrame: 合并后的数据框
    """
    all_data = []
    
    # 获取所有CSV文件
    csv_files = glob.glob(os.path.join(data_dir, "*.csv"))
    
    for file_path in csv_files:
        try:
            df = pd.read_csv(file_path)
            if len(df) > 1:  # 只处理有数据的文件
                all_data.append(df)
        except Exception as e:
            print(f"Error loading {file_path}: {e}")
    
    if all_data:
        combined_df = pd.concat(all_data, ignore_index=True)
        return combined_df
    else:
        return pd.DataFrame()


def cox_regression_synthetic(df, method1="Random", method2="3phstar", duration_col="Sec"):
    """
    使用Cox回归分析比较两种方法在synthetic数据上的性能差异
    
    Args:
        df: 数据框
        method1: 第一种方法名称
        method2: 第二种方法名称
        duration_col: 持续时间列名 (Sec)
    
    Returns:
        tuple: (z_value, p_value)
    """
    
    # 过滤出指定方法的数据
    filtered_df = df[df["Method"].isin([method1, method2])].copy()
    
    if len(filtered_df) == 0:
        print(f"No data found for methods {method1} and {method2}")
        return None, None
    
    # 创建分组变量：0表示method1，1表示method2
    filtered_df["Group"] = filtered_df["Method"].map({method1: 0, method2: 1})
    
    # 为Cox回归添加事件列（假设所有观测都是完整的事件，没有删失）
    filtered_df["Event"] = 1
    
    # 准备Cox模型数据
    cox_data = filtered_df[[duration_col, "Group", "Benchmark", "Event"]].copy()
    
    try:
        # 拟合Cox模型，使用Benchmark作为分层变量
        cph = CoxPHFitter()
        cph.fit(cox_data, duration_col=duration_col, event_col="Event", strata="Benchmark")
        
        # 获取结果
        z_value = cph.summary.loc["Group", "z"]
        p_value = cph.summary.loc["Group", "p"]
        
        return z_value, p_value
        
    except Exception as e:
        print(f"Error in Cox regression: {e}")
        return None, None


def synthetic_cox_analysis():
    """
    对synthetic数据进行Cox回归分析
    """
    print("="*80)
    print("SYNTHETIC DATA COX REGRESSION ANALYSIS")
    print("="*80)
    
    # 加载数据
    df = load_synthetic_data()
    
    if df.empty:
        print("No synthetic data found!")
        return
    
    print(f"Loaded {len(df)} records from synthetic data")
    print(f"Benchmarks: {sorted(df['Benchmark'].unique())}")
    print(f"Methods: {sorted(df['Method'].unique())}")
    
    # 整体分析：Random vs 3phstar
    print(f"\n{'='*60}")
    print("OVERALL COMPARISON: Random vs 3phstar")
    print(f"{'='*60}")
    
    z_value, p_value = cox_regression_synthetic(df, "Random", "3phstar")
    
    if z_value is not None:
        print(f"Z-value: {z_value:.4f}")
        print(f"P-value: {p_value:.6f}")
        
        if p_value < 0.001:
            significance = "***"
        elif p_value < 0.01:
            significance = "**"
        elif p_value < 0.05:
            significance = "*"
        else:
            significance = "ns"
            
        print(f"Significance: {significance}")
        
        # 解释结果
        if z_value > 0:
            print("Interpretation: 3phstar tends to be faster than Random")
        else:
            print("Interpretation: Random tends to be faster than 3phstar")
    
    # 分benchmark分析
    print(f"\n{'='*60}")
    print("BY BENCHMARK ANALYSIS")
    print(f"{'='*60}")
    
    benchmarks = sorted(df["Benchmark"].unique())
    
    for benchmark in benchmarks:
        benchmark_data = df[df["Benchmark"] == benchmark]
        methods = benchmark_data["Method"].unique()
        
        if "Random" in methods and "3phstar" in methods:
            print(f"\n--- {benchmark} ---")
            z_val, p_val = cox_regression_synthetic(benchmark_data, "Random", "3phstar")
            
            if z_val is not None:
                if p_val < 0.001:
                    sig = "***"
                elif p_val < 0.01:
                    sig = "**"
                elif p_val < 0.05:
                    sig = "*"
                else:
                    sig = "ns"
                
                winner = "3phstar" if z_val > 0 else "Random"
                print(f"Z={z_val:.4f}, P={p_val:.6f} ({sig}) - {winner} faster")
        else:
            print(f"\n--- {benchmark} ---")
            print("Missing Random or 3phstar data")


def cox_regression_test():
    if _load_buggy_data is None:
        print("buggy helpers not available; skipping buggy data analysis")
        return

    try:
        (
            df_buggy,
            benchmarks,
            captions,
            syntactic_benchs,
            rff_benchs,
            memory_bugs,
            assertion_failures,
            seeded_data_structures,
            hard_benchmarks,
        ) = _load_buggy_data("out/buggy.csv")

        dfs = [assertion_failures]
        names = ["assertion_failures"]

        for b, name in zip(dfs, names):
            filtered_df = df_buggy[df_buggy["Benchmark"].isin(b)]
            print(f"Running cox_regression on {name}")
            z_value, p_value = cox_regression(filtered_df, "Random", "rfcostarddag")
    except Exception as e:
        print(f"Error in buggy data analysis: {e}")
        print("Skipping buggy data analysis")

    # # 初始化结果矩阵
    # n = len(captions)
    # z_p_matrix = [["" for _ in range(n)] for _ in range(n)]

    # # Cox 回归两两对比
    # for i, version1 in enumerate(captions):
    #     for j, version2 in enumerate(captions):
    #         if i == j:
    #             z_p_matrix[i][j] = "--"  # 对角线表示自己
    #         else :  # 避免重复计算
    #             z_value, p_value = cox_regression(filtered_df, version1, version2)
    #             # 填入表格
    #             z_p_matrix[i][j] = f"{z_value:.2f}/{p_value:.2e}"

    # # 生成带颜色的 LaTeX 表格
    # generate_latex_table_with_color(z_p_matrix, captions, output_file="stat.tex")




if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Benchmark-set analyses")
    parser.add_argument("--dir", dest="data_dir", default="out/synthetic/", help="Directory containing benchmark-set CSVs")
    parser.add_argument("--m1", dest="m1", default="Random", help="Method 1 (baseline)")
    parser.add_argument("--m2", dest="m2", default="3phstar", help="Method 2 (treatment)")
    parser.add_argument("--duration", dest="duration", default="Sec", help="Duration column to use (default: Sec)")
    parser.add_argument("--by-benchmark", dest="by_bench", action="store_true", help="Also print per-benchmark results")
    parser.add_argument("--analysis", choices=["cox", "coverage"], default="cox", help="Which analysis to run")
    args = parser.parse_args()

    # Load from path and run selected analysis for the whole benchmark set
    def _load_benchmark_set(data_dir: str):
        csvs = []
        for root, _, files in os.walk(data_dir):
            for fn in files:
                if fn.endswith('.csv'):
                    csvs.append(os.path.join(root, fn))
        frames = []
        for p in csvs:
            try:
                df = pd.read_csv(p)
                # Expect columns: Benchmark, Method, Iter?, Sec
                expected_cols = {"Benchmark", "Method", args.duration}
                if expected_cols.issubset(df.columns):
                    frames.append(df[["Benchmark", "Method", args.duration]].copy())
            except Exception:
                pass
        return pd.concat(frames, ignore_index=True) if frames else pd.DataFrame()

    if args.analysis == "cox":
        df_set = _load_benchmark_set(args.data_dir)
        if df_set.empty:
            print(f"No usable CSVs found under {args.data_dir}")
            sys.exit(1)

        # Prepare data for Cox
        # Ensure numeric duration and drop invalid
        df_set[args.duration] = pd.to_numeric(df_set[args.duration], errors='coerce')
        df_set = df_set.dropna(subset=[args.duration])
        df_set = df_set[df_set[args.duration] > 0]

        # Filter methods
        method_col = "Method" if "Method" in df_set.columns else ("Caption" if "Caption" in df_set.columns else None)
        if method_col is None:
            print("No Method/Caption column found in input CSVs")
            sys.exit(1)
        df_set = df_set.rename(columns={method_col: "Method"})
        df_methods = df_set[df_set["Method"].isin([args.m1, args.m2])].copy()
        if df_methods.empty:
            print(f"No records for methods {args.m1} and {args.m2} under {args.data_dir}")
            sys.exit(1)

        df_methods["Group"] = df_methods["Method"].map({args.m1: 0, args.m2: 1})
        df_methods["Event"] = 1

        cox_data = df_methods[[args.duration, "Group", "Benchmark", "Event"]]
        cph = CoxPHFitter()
        cph.fit(cox_data, duration_col=args.duration, event_col="Event", strata="Benchmark")

        z_value = cph.summary.loc["Group", "z"]
        p_value = cph.summary.loc["Group", "p"]
        hr = cph.summary.loc["Group", "exp(coef)"]

        print("="*80)
        print(f"Benchmark set: {args.data_dir}")
        print(f"Methods: {args.m1} vs {args.m2} | Duration: {args.duration}")
        print(f"Z: {z_value:.4f}  P: {p_value:.6g}  HR: {hr:.4f}")
        if p_value < 0.001:
            sig = "***"
        elif p_value < 0.01:
            sig = "**"
        elif p_value < 0.05:
            sig = "*"
        else:
            sig = "ns"
        print(f"Significance: {sig}")
        print("Faster:", args.m2 if z_value > 0 else args.m1)

        if args.by_bench:
            print("\nPer-benchmark results:")
            for bmk in sorted(df_methods["Benchmark"].unique()):
                sub = df_methods[df_methods["Benchmark"] == bmk]
                if set([args.m1, args.m2]).issubset(set(sub["Method"].unique())):
                    sub_cph = CoxPHFitter()
                    sub_data = sub[[args.duration, "Group", "Event"]]
                    # No strata within a single benchmark
                    sub_cph.fit(sub_data, duration_col=args.duration, event_col="Event")
                    z = sub_cph.summary.loc["Group", "z"]
                    p = sub_cph.summary.loc["Group", "p"]
                    print(f"  {bmk}: Z={z:.3f}  P={p:.3g}")
            print()
    else:
        coverage_mann_whitney_analysis(args.data_dir, args.m1, args.m2)
