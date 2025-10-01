

from figure1 import benchmarks,  df

current_bench = benchmarks[0]


benchmarks = [f"{b}({i})" for b in benchmarks for i in range(3, 9)]


def split_list(lst):
    mid = len(lst) // 2
    return lst[:mid], lst[mid:]


benchmarks_1, benchmarks_2 = split_list(benchmarks)

def print_bench_line(benchname):
    line = benchname
    method_data = {}  # 存储每个method的数据用于比较
    
    # 先收集所有method的数据
    for method in ["no", "star" ,"3phstar"]:
        dff = df[(df["Method"] == method) & (df["Benchmark"] == benchname)].head(30)
        sec = dff["Sec"]
        
        dff_data = dff.dropna()
        sec_data = dff_data["Sec"]
        
        # 计算统计数据
        has_data = len(sec_data) > 0
        mean_sec = sec_data.mean() if has_data else float('inf')
        std_sec = sec_data.std() if has_data and len(sec_data) > 1 else 0
        percent = len(sec_data) / len(sec) * 100 if len(sec) > 0 else 0
        percent_rounded = round(percent * 30) / 30
        
        # 存储数据用于比较
        method_data[method] = {
            'has_data': has_data,
            'mean': mean_sec,
            'std': std_sec,
            'percent': percent_rounded,
            'sec_str': r" \clock " if not has_data else f" {mean_sec:.1f} " + (r"$\pm$" + f" {std_sec:.1f} " if len(sec_data) > 1 else "")
        }
    
    # 找到最佳method（按优先级：percentage最高 > mean最小 > std最小）
    best_method = None
    best_score = (-float('inf'), float('inf'), float('inf'))  # (percent, mean, std)
    
    for method, data in method_data.items():
        if data['has_data']:
            score = (data['percent'], -data['mean'], -data['std'])  # 百分比越高越好，mean/std越小越好
            if score > best_score:
                best_score = score
                best_method = method
    
    # 生成输出行
    for method in ["no", "star" ,"3phstar"]:
        data = method_data[method]
        
        # 如果是最佳method，加粗
        if method == best_method and data['has_data']:
            sec_str = r" \textbf{\text{" + data['sec_str'] + r"}}"  # 移除原有的$，在外层加
            percent_str = r" \textbf{" + f"{data['percent']:.1f}" + "}"
        else:
            sec_str = data['sec_str']
            percent_str = f"{data['percent']:.1f}"
        
        line += "&" + sec_str
        line += f" & {percent_str}"
    
    line += r"\\"
    print(line)
    
    
def print_bench_line1(benchname):
    # print(bench)
    line = benchname
    for method in ["no", "star" ,"3phstar"]:
        dff = df[(df["Method"] == method) & (df["Benchmark"] == benchname)].head(30)
        sec = dff["Sec"]

        dff_data = dff.dropna()
        # line += f" {len(dff_data)}/{len(dff)} "
        sec_data = dff_data["Sec"]

        sec_str = r" \clock " if len(sec_data) == 0 else f" {sec_data.mean():.1f} "
        if len(sec_data) > 1:
            sec_str += r"$\pm$" + f" {sec_data.std():.1f} "
        line += "&" + sec_str

        percent = len(sec_data) / len(sec) * 100
        percent = round(percent * 30) / 30
        line += f" & {percent:.1f}"
        # line += f" & {len(sec_data)}/{len(sec)}"

    line += r"\\"
    print(line)


def print_bench_lines(benchs, curr):
    for b in benchs:

        if not b.startswith(curr + r"("):
            print(r"\hline")
            curr = b.split("(")[0]

        print_bench_line(b)

    return curr


def generate_table(current_bench):

    print(r"\begin{table}[htbp]")
    print(r"\centering")
    print(r"\caption{\algname's heuristics are useful for bug finding (1/2)}")
    print(r"\label{tab:3-weazer-time-variations-1}")
    # print(r"\resizebox{\columnwidth}{!}{")
    print(r"\begin{tabular}{lcccccc}")
    print(r"\hline")
    print(
        r"\multirow{2}{*}{\textbf{Benchmark}}    & \multicolumn{2}{c}{\textbf{\algnamenomaxnoval}}    & \multicolumn{2}{c}{\textbf{\algnamenoval}}    & \multicolumn{2}{c}{\textbf{$\algname$}}\\\cline{2-7}"
    )
    print(r" & Sec        & \%       & Sec        & \%       & Sec        & \%")
    print(r" \\ \hline")

    current_bench = print_bench_lines(benchmarks_1, current_bench)
    print(r"\hline")
    print(r"\end{tabular}")
    print(r"\end{table}")

    print(r"\begin{table}[htbp]")
    print(r"\centering")
    print(r"\caption{\algname's heuristics are useful for bug finding (2/2)}")
    print(r"\label{tab:3-weazer-time-variations-2}")
    # print(r"\resizebox{\columnwidth}{!}{")
    print(r"\begin{tabular}{lcccccc}")
    print(r"\hline")
    print(
        r"\multirow{2}{*}{\textbf{Benchmark}}    & \multicolumn{2}{c}{\textbf{\algnamenomaxnoval}}    & \multicolumn{2}{c}{\textbf{\algnamenoval}}    & \multicolumn{2}{c}{\textbf{$\algname$}}\\\cline{2-7}"
    )
    print(r" & Sec        & \%       & Sec        & \%       & Sec        & \%")
    print(r" \\ \hline")

    current_bench = print_bench_lines(benchmarks_2, current_bench)
    print(r"\hline")
    
    print(r"\end{tabular}")
    print(r"\end{table}")


if __name__ == "__main__":
    generate_table(current_bench)
    pass
