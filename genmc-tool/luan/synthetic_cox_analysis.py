import pandas as pd
import numpy as np
import os
import glob
from lifelines import CoxPHFitter
from scipy import stats


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
                print(f"Loaded {len(df)} records from {os.path.basename(file_path)}")
        except Exception as e:
            print(f"Error loading {file_path}: {e}")
    
    if all_data:
        combined_df = pd.concat(all_data, ignore_index=True)
        print(f"Total records loaded: {len(combined_df)}")
        return combined_df
    else:
        print("No data loaded!")
        return pd.DataFrame()


def cox_regression_analysis(df, method1="Random", method2="3phstar", duration_col="Sec"):
    """
    使用Cox回归分析比较两种方法的性能差异
    
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
    
    print(f"Analyzing {len(filtered_df)} records:")
    print(f"  {method1}: {len(filtered_df[filtered_df['Method'] == method1])} records")
    print(f"  {method2}: {len(filtered_df[filtered_df['Method'] == method2])} records")
    
    # 创建分组变量：0表示method1，1表示method2
    filtered_df["Group"] = filtered_df["Method"].map({method1: 0, method2: 1})
    
    # 为Cox回归添加事件列（假设所有观测都是完整的事件，没有删失）
    filtered_df["Event"] = 1
    
    # 准备Cox模型数据
    cox_data = filtered_df[[duration_col, "Group", "Benchmark", "Event"]].copy()
    
    # 检查数据
    print(f"Cox regression data shape: {cox_data.shape}")
    print(f"Duration column ({duration_col}) statistics:")
    print(cox_data[duration_col].describe())
    
    try:
        # 拟合Cox模型，使用Benchmark作为分层变量
        cph = CoxPHFitter()
        cph.fit(cox_data, duration_col=duration_col, event_col="Event", strata="Benchmark")
        
        # 获取结果
        z_value = cph.summary.loc["Group", "z"]
        p_value = cph.summary.loc["Group", "p"]
        hazard_ratio = cph.summary.loc["Group", "exp(coef)"]
        
        print("\n" + "="*60)
        print(f"Cox Regression Results: {method1} vs {method2}")
        print("="*60)
        print(f"Z-value: {z_value:.4f}")
        print(f"P-value: {p_value:.6f}")
        print(f"Hazard Ratio: {hazard_ratio:.4f}")
        
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
            faster_method = method2
            slower_method = method1
        else:
            faster_method = method1
            slower_method = method2
            
        print(f"Interpretation: {faster_method} tends to be faster than {slower_method}")
        print("="*60)
        
        return z_value, p_value
        
    except Exception as e:
        print(f"Error in Cox regression: {e}")
        return None, None


def analyze_by_benchmark_groups(df):
    """
    按benchmark分组进行分析
    """
    print("\n" + "="*80)
    print("ANALYSIS BY BENCHMARK GROUPS")
    print("="*80)
    
    # 获取所有benchmark类型
    benchmarks = df["Benchmark"].unique()
    print(f"Found benchmarks: {benchmarks}")
    
    results = {}
    
    for benchmark in benchmarks:
        print(f"\n--- Analysis for {benchmark} ---")
        benchmark_data = df[df["Benchmark"] == benchmark]
        
        # 检查是否有Random和3phstar数据
        methods = benchmark_data["Method"].unique()
        if "Random" in methods and "3phstar" in methods:
            z_val, p_val = cox_regression_analysis(benchmark_data, "Random", "3phstar")
            if z_val is not None:
                results[benchmark] = {"z": z_val, "p": p_val}
        else:
            print(f"Missing Random or 3phstar data for {benchmark}")
    
    return results


def overall_analysis(df):
    """
    整体分析所有数据
    """
    print("\n" + "="*80)
    print("OVERALL ANALYSIS (ALL BENCHMARKS COMBINED)")
    print("="*80)
    
    z_val, p_val = cox_regression_analysis(df, "Random", "3phstar")
    return z_val, p_val


def main():
    """
    主函数：执行完整的分析流程
    """
    print("Loading synthetic data...")
    df = load_synthetic_data()
    
    if df.empty:
        print("No data to analyze!")
        return
    
    print(f"\nData overview:")
    print(f"Total records: {len(df)}")
    print(f"Benchmarks: {df['Benchmark'].unique()}")
    print(f"Methods: {df['Method'].unique()}")
    print(f"Sec column statistics:")
    print(df["Sec"].describe())
    
    # 整体分析
    overall_z, overall_p = overall_analysis(df)
    
    # 分组分析
    benchmark_results = analyze_by_benchmark_groups(df)
    
    # 汇总结果
    print("\n" + "="*80)
    print("SUMMARY OF RESULTS")
    print("="*80)
    
    if overall_z is not None:
        print(f"Overall comparison (Random vs 3phstar):")
        print(f"  Z-value: {overall_z:.4f}")
        print(f"  P-value: {overall_p:.6f}")
    
    print(f"\nBy benchmark:")
    for benchmark, result in benchmark_results.items():
        print(f"  {benchmark}: Z={result['z']:.4f}, P={result['p']:.6f}")


if __name__ == "__main__":
    main()