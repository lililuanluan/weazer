import pandas as pd
import matplotlib.pyplot as plt
import glob

benchmarks = [
    "dglm-queue",
    "dglm-queue2",
    "dglm-queue3",
    "ms-queue",
    "ms-queue2",
    "ms-queue3",
    "ms-queue-opt",
    "ms-queue-opt2",
    "ms-queue-opt3",
    "treiber-stack",
    "treiber-stack2",
    "treiber-stack3",
    "optimized-lf-queue",
    "optimized-lf-queue2",
    "optimized-lf-queue3",
]


file_list = glob.glob("out/buggy/data/4versions-*.csv")


df = pd.concat([pd.read_csv(file) for file in file_list], ignore_index=True)

df["Sec"] = pd.to_numeric(df["Sec"], errors="coerce")



def plot_figure(bench):

    plt.figure(figsize=(10, 6))
    all_seconds = []

    prefix = f"{bench}" + "\("
    # print(prefix)
    filtered_random = df[
        (df["Method"] == "Random") & (df["Benchmark"].str.contains(prefix, na=False))
    ]
    filtered_3phstar = df[
        (df["Method"] == "3phstar") & (df["Benchmark"].str.contains(prefix, na=False))
    ]

    

    if filtered_random.empty & filtered_3phstar.empty:
        return

    for i in range(3, 9):
        benchname = f"{bench}({i})"
        # print(f">>> {benchname}")
        rand_data = (
            filtered_random[filtered_random["Benchmark"] == benchname].head(30).dropna()
        )
        fuzz_data = (
            filtered_3phstar[filtered_3phstar["Benchmark"] == benchname]
            .head(30)
            .dropna()
        )


        random_seconds = rand_data["Sec"]
        fuzz_seconds = fuzz_data["Sec"]

        all_seconds.extend(random_seconds.tolist())
        all_seconds.extend(fuzz_seconds.tolist())
        all_seconds.extend([random_seconds.mean() + random_seconds.std()])
        all_seconds.extend([fuzz_seconds.mean() + fuzz_seconds.std()])

        diff = 0.1
        plt.errorbar(
            i - diff,
            random_seconds.mean(),
            yerr=random_seconds.std(),
            fmt="o",
            color="blue",
            capsize=8,
            markersize=8,
        )

        plt.errorbar(
            i + diff,
            fuzz_seconds.mean(),
            yerr=fuzz_seconds.std(),
            fmt="o",
            color="red",
            capsize=8,
            markersize=9,
        )

       

    if all_seconds:
        max_y = max(all_seconds)
        plt.ylim(-max_y * 0.1, max_y)
    else:
        plt.ylim(0, 1)

    plt.title(bench, fontsize=40, fontfamily="serif")

    labels = [f"{i}" for i in range(3, 9)]
    plt.xticks(range(3, 9), labels, ha="center", fontsize=20)
    plt.yticks(fontsize=13)
    plt.ylabel("Sec", fontsize=25)

    plt.gca().tick_params(axis="x", which="both", length=0)
    plt.gca().tick_params(axis="y", which="both", length=0)
    plt.tight_layout()

    output_file = f"out/buggy/plots/{bench}.pdf"
    # print(f"Plot saving to {output_file}")
    plt.savefig(output_file, format="pdf")



def plot_figures():
    for bench in benchmarks:
        plot_figure(bench=bench)


if __name__ == "__main__":
    plot_figures()
