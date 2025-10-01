import os
import re
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path
from scipy import interpolate


def load_files(benchname, folder_path):
    all_files = os.listdir(folder_path)
    # 定义匹配模式
    random_pattern = re.compile(rf"^{re.escape(benchname)}-Random-(\d+)$")
    star_pattern = re.compile(rf"^{re.escape(benchname)}-3phstar-(\d+)$")
    random_files = []
    star_files = []

    for filename in all_files:
        match = random_pattern.match(filename)
        if match:
            random_files.append((int(match.group(1)), filename))

        match = star_pattern.match(filename)
        if match:
            star_files.append((int(match.group(1)), filename))

    return random_files, star_files


def get_benchnames(folder_path):
    benchnames = set()
    pattern = r"^(.+)-(Random|3phstar)-(\d+)(?:\.\w+)?$"
    for filename in os.listdir(folder_path):
        if os.path.isfile(os.path.join(folder_path, filename)):
            match = re.match(pattern, filename)
            if match:
                benchnames.add(match.group(1))
    return sorted(list(benchnames))


def load_benchmark_data(benchname, folder_path="out/buggy/coverage/"):
    benchmark_data = {benchname: {"Random": [], "3phstar": []}}

    random_files, star_files = load_files(benchname, folder_path)

    # 按序号排序并读取
    for i, filename in sorted(random_files, key=lambda x: x[0]):
        filepath = os.path.join(folder_path, filename)
        try:
            df = pd.read_csv(filepath)
            benchmark_data[benchname]["Random"].append(df)
        except Exception as e:
            print(f'Error reading "{filename}": {e}')

    # 查找并读取3phstar方法的文件

    # 按序号排序并读取
    for i, filename in sorted(star_files, key=lambda x: x[0]):
        filepath = os.path.join(folder_path, filename)
        try:
            df = pd.read_csv(filepath)
            benchmark_data[benchname]["3phstar"].append(df)
        except Exception as e:
            print(f'Error reading "{filename}": {e}')

    return benchmark_data


def process_method_data_corrected(data_list, time_points=1000):
    """正确处理每个时间点的均值和标准差，只包含仍在运行的测试"""
    if not data_list:
        return None, None, None

    # 找到所有数据的时间范围
    all_end_times = [df["SecElapsed"].iloc[-1] for df in data_list]
    max_end_time = max(all_end_times)

    # 创建统一的时间点
    uniform_times = np.linspace(0, max_end_time, time_points)

    # 为每个时间点收集仍在运行的测试的数据
    cover_values_at_times = []

    for t in uniform_times:
        covers_at_t = []
        for df in data_list:
            # 找到这个测试在时间t时的覆盖度
            # 如果测试在时间t之前已经结束，使用最后一个值
            if t <= df["SecElapsed"].iloc[-1]:
                # 找到时间t对应的覆盖度（插值或最近的值）
                idx = np.searchsorted(df["SecElapsed"].values, t)
                if idx == 0:
                    cover = df["Cover"].iloc[0]
                elif idx == len(df):
                    cover = df["Cover"].iloc[-1]
                else:
                    # 线性插值
                    t0, t1 = df["SecElapsed"].iloc[idx - 1], df["SecElapsed"].iloc[idx]
                    c0, c1 = df["Cover"].iloc[idx - 1], df["Cover"].iloc[idx]
                    if t1 > t0:  # 避免除零
                        cover = c0 + (c1 - c0) * (t - t0) / (t1 - t0)
                    else:
                        cover = c0
                covers_at_t.append(cover)

        cover_values_at_times.append(covers_at_t)

    # 计算每个时间点的均值和标准差
    mean_cover = []
    std_cover = []

    for covers_at_t in cover_values_at_times:
        if covers_at_t:
            mean_cover.append(np.mean(covers_at_t))
            std_cover.append(np.std(covers_at_t))
        else:
            mean_cover.append(0)
            std_cover.append(0)

    return uniform_times, np.array(mean_cover), np.array(std_cover)


def process_method_data_efficient(data_list, time_points=1000):
    """更高效的处理方法：预先插值所有测试到统一时间点"""
    if not data_list:
        return None, None, None

    # 找到所有测试的结束时间
    end_times = [df["SecElapsed"].iloc[-1] for df in data_list]
    max_end_time = max(end_times)

    # 创建统一的时间点
    uniform_times = np.linspace(0, max_end_time, time_points)

    # 对每个测试进行插值
    interpolated_covers = []
    for df in data_list:
        df_sorted = df.sort_values("SecElapsed")
        times = df_sorted["SecElapsed"].values
        covers = df_sorted["Cover"].values

        # 只在测试运行的时间范围内插值，之后设为NaN
        f = interpolate.interp1d(
            times, covers, kind="linear", bounds_error=False, fill_value=np.nan
        )
        interp_covers = f(uniform_times)
        interpolated_covers.append(interp_covers)

    # 转换为数组
    interp_array = np.array(interpolated_covers)

    # 计算每个时间点的均值和标准差（忽略NaN值）
    mean_cover = np.zeros_like(uniform_times)
    std_cover = np.zeros_like(uniform_times)

    for i in range(len(uniform_times)):
        # 获取当前时间点的所有非NaN值
        valid_values = interp_array[:, i]
        valid_values = valid_values[~np.isnan(valid_values)]

        if len(valid_values) > 0:
            mean_cover[i] = np.mean(valid_values)
            if len(valid_values) > 1:  # 需要至少2个值来计算标准差
                std_cover[i] = np.std(valid_values)
            else:
                std_cover[i] = 0
        else:
            mean_cover[i] = np.nan
            std_cover[i] = np.nan

    return uniform_times, mean_cover, std_cover


def plot_benchmark_comparison(benchmark_data, output_dir="out/buggy/coverage_plots"):
    os.makedirs(output_dir, exist_ok=True)

    for benchname, data in benchmark_data.items():
        plt.figure(figsize=(10, 6))

        # 处理Random方法数据
        if data["Random"]:
            times_random, mean_random, std_random = process_method_data_efficient(
                data["Random"]
            )
            if times_random is not None:
                # 移除NaN值
                valid_mask = ~np.isnan(mean_random)
                valid_times = times_random[valid_mask]
                valid_mean = mean_random[valid_mask]
                valid_std = std_random[valid_mask]

                plt.plot(
                    valid_times, valid_mean, color="blue", linewidth=2, label="Random"
                )
                plt.fill_between(
                    valid_times,
                    valid_mean - valid_std,
                    valid_mean + valid_std,
                    color="blue",
                    alpha=0.3,
                )
                # return
        else:
            print("NO RANDOM DATA!")

        # 处理3phstar方法数据
        if data["3phstar"]:

            times_star, mean_star, std_star = process_method_data_efficient(
                data["3phstar"]
            )
            if times_star is not None:
                # 移除NaN值
                valid_mask = ~np.isnan(mean_star)
                valid_times = times_star[valid_mask]
                valid_mean = mean_star[valid_mask]
                valid_std = std_star[valid_mask]

                plt.plot(
                    valid_times, valid_mean, color="red", linewidth=2, label="3phstar"
                )
                plt.fill_between(
                    valid_times,
                    valid_mean - valid_std,
                    valid_mean + valid_std,
                    color="red",
                    alpha=0.3,
                )
        else:
            print("NO 3PHSTAR DATA!")

        plt.xlabel("Sec", fontsize=12)
        plt.ylabel("# Distinct Graphs", fontsize=12)
        plt.title(benchname, fontsize=14)

        plt.tight_layout()
        plt.savefig(
            os.path.join(output_dir, f"{benchname}_coverage_plot.pdf"),
            bbox_inches="tight",
        )
        plt.close()

        print(f'Generated "{output_dir}/{benchname}_coverage_plot.pdf"')


def plot_benchmark(benchname):
    benchmark_data = load_benchmark_data(benchname)
    plot_benchmark_comparison(benchmark_data)


def main():
    folder_path = "out/buggy/coverage/"
    benchmarks = get_benchnames(folder_path)
    print(benchmarks)
    for b in benchmarks:
        print(f">>> {b}")
        plot_benchmark(b)


if __name__ == "__main__":
    main()
