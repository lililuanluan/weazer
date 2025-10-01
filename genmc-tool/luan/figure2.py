import pandas as pd
import numpy as np
import os
import re
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

MAXN = 10000

def plot_coverage_time(benchname, plot_dir, combined_datas, colors, df_verify):
    match = df_verify[df_verify["Benchmark"] == benchname]
    iter_val = match.iloc[0]["Iter"]
    plt.figure(figsize=(10, 6))
    
    # 找到所有时间点中的最大值
    max_time = max(max(data["time"]) for data in combined_datas.values()) if combined_datas else 100
    
    for cap, data in combined_datas.items():
        # 获取时间和覆盖率数据
        time = data["time"]
        coverage = data["coverage"]
        std = data["std"]
        
        # 绘制曲线
        plt.plot(time, coverage, label=cap.capitalize(), color=colors[cap])
        plt.fill_between(time, coverage - std, coverage + std, color=colors[cap], alpha=0.2)
        
        # 计算覆盖率百分比
        if np.isnan(iter_val):
            percentage_text = ""
        else:
            percentage_text = f"{coverage[-1] / int(iter_val) * 100:.2f}%"
            
        # 确定标签位置
        point = 8000 if cap == 'fuzz' else 14000
        percent_font = 30
        
        # 获取标签位置
        if len(time) > point:
            label_time = time[point]
            label_coverage = coverage[point]
        else:
            label_time = time[-1]
            label_coverage = coverage[-1]
        
        # 调整标签位置
        y_min, y_max = plt.ylim()
        y_range = y_max - y_min
        diff = y_range * 0.05 if cap == "fuzz" else -y_range * 0.05
        
        # 对齐方式
        V = 'bottom' if cap == 'fuzz' else 'top'
        H = 'right' if cap == 'fuzz' else 'left'
        
        # 添加百分比标签
        plt.text(label_time, label_coverage + diff, percentage_text, 
                 fontsize=percent_font, color=colors[cap],
                 verticalalignment=V, horizontalalignment=H)
    
    # 设置标题
    plt.title(f"{benchname}", fontsize=40, fontfamily="serif")
    
    # 添加顶部百分比标签
    y_offset = 1.05
    for cap, data in combined_datas.items():
        coverage = data["coverage"]
        if np.isnan(iter_val):
            percentage_text = "-%"
        else:
            percentage_text = f"{coverage[-1] / int(iter_val) * 100:.2f}%"
        
        plt.suptitle(f" {percentage_text}", fontsize=25, color=colors[cap], 
                     x=0.5, y=y_offset, ha='center')
        y_offset += 0.03
    
    # 设置坐标标签
    plt.xlabel("Time (seconds)", fontsize=25)
    plt.ylabel("# Distinct Graphs", fontsize=25)
    
    # 设置坐标轴范围
    plt.ylim(0, plt.ylim()[1] * 1.1)
    plt.xlim(0, max_time * 1.02)
    
    # 使用科学计数法格式化时间轴
    plt.gca().xaxis.set_major_formatter(ticker.FormatStrFormatter('%.0f'))
    
    # 移除y轴刻度
    plt.yticks([])
    
    # 保存图像
    output_filename = f"{benchname}_coverage_time_plot.pdf"
    output = os.path.join(plot_dir, output_filename)
    plt.savefig(output)
    print(f'Plot saved as "{output}"')
    plt.close()
    
def get_benchnames(directory):
    # 在directory路径下的文件格式为 <benchname>-method-i
    # 且benchname中可能有 '-'
    # 文件没有csv后缀
    benchnames = set()
    for filename in os.listdir(directory):
        parts = filename.split("-")
        if len(parts) >= 3:
            benchname = "-".join(parts[:-2])
            benchnames.add(benchname)
    print(f"benchmarks found: {benchnames}")
    return sorted(benchnames)

def main():
    # 读取覆盖率数据
    df_verify = pd.read_csv("out/verify.csv")
 
    df_verify["Iter"] = pd.to_numeric(df_verify["Iter"], errors="coerce")
    benchmarks = get_benchnames("out/coverage/")
    captions = ["rand", "fuzz"]

    colors = {"rand": "blue", "fuzz": "red"}
    coverage_dir = "out/coverage/"
    plot_dir = "out/coverage/plots/"
    
    if not os.path.exists(plot_dir):
        os.makedirs(plot_dir)

    for benchname in benchmarks:
        print(f"Processing {benchname}")
        matched_files = {"rand": [], "fuzz": []}
        time_files = {"rand": [], "fuzz": []}
        
        # 收集数据文件和时间文件
        for filename in os.listdir(coverage_dir):
            # 跳过time文件
            if "-time" in filename:
                continue
                
            match = re.match(rf"{re.escape(benchname)}-(\w+)-\d+", filename)
            if match:
                cap = match.group(1)
                filepath = os.path.join(coverage_dir, filename)
                matched_files[cap].append(filepath)
                
                # 查找对应的时间文件
                time_filename = filepath 
                if os.path.exists(time_filename):
                    time_files[cap].append(time_filename)
                else:
                    print(f"Warning: Time file not found for {filename}")
        
        combined_datas = {}
        for cap, data_files in matched_files.items():
            if data_files and time_files[cap]:
                # 合并同一方法的所有运行数据
                all_times = []
                all_coverages = []
                
                for i, (data_file, time_file) in enumerate(zip(data_files, time_files[cap])):
                    try:
                        # 修复1: 跳过标题行读取数据
                        df_data = pd.read_csv(data_file, skiprows=1, header=None, names=["iter", "coverage"])
                        df_data["iter"] = pd.to_numeric(df_data["iter"], errors="coerce")
                        df_data["coverage"] = pd.to_numeric(df_data["coverage"], errors="coerce")
                        
                        # 修复2: 跳过标题行读取时间数据
                        df_time = pd.read_csv(time_file, skiprows=1, header=None, names=["iter", "time"])
                        df_time["iter"] = pd.to_numeric(df_time["iter"], errors="coerce")
                        df_time["time"] = pd.to_numeric(df_time["time"], errors="coerce")
                        
                        # 在时间数据前添加0时刻 (iter=0, time=0.0)
                        initial_time = pd.DataFrame({"iter": [0], "time": [0.0]})
                        df_time = pd.concat([initial_time, df_time]).reset_index(drop=True)
                        
                        # 确保数据文件和时间文件的迭代次数匹配
                        if len(df_data) != len(df_time):
                            min_len = min(len(df_data), len(df_time))
                            df_data = df_data.iloc[:min_len]
                            df_time = df_time.iloc[:min_len]
                            # print(f"Warning: Data length mismatch for {benchname}-{cap}-{i}, truncated to {min_len} iterations")
                        
                        # 合并数据
                        merged = pd.merge(df_data, df_time, on="iter", how="inner")
                        
                        # 添加到列表
                        all_times.append(merged["time"].values)
                        all_coverages.append(merged["coverage"].values)
                    except Exception as e:
                        print(f"Error processing {data_file}: {str(e)}")
                        continue
                
                if not all_times:
                    print(f"No valid data for {benchname}-{cap}")
                    continue
                
                # 对齐所有运行的时间点
                min_length = min(len(t) for t in all_times)
                aligned_times = [t[:min_length] for t in all_times]
                aligned_coverages = [c[:min_length] for c in all_coverages]
                
                # 计算平均值和标准差
                avg_time = np.mean(aligned_times, axis=0)
                avg_coverage = np.mean(aligned_coverages, axis=0)
                std_coverage = np.std(aligned_coverages, axis=0)
                
                # 存储结果
                combined_datas[cap] = {
                    "time": avg_time,
                    "coverage": avg_coverage,
                    "std": std_coverage
                }
        
        if combined_datas:
            plot_coverage_time(
                benchname=benchname,
                plot_dir=plot_dir,
                combined_datas=combined_datas,
                colors=colors,
                df_verify=df_verify
            )
        else:
            print(f"No valid data found for {benchname}")

if __name__ == "__main__":
    main()