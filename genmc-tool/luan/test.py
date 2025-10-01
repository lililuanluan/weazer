# change this list to python list syntax
synthetic_benchmarks=[

	"long-assert(5)",
	"long-assert(6)",
	"long-assert(7)",
	"long-assert(2)",
	"long-assert(3)",
	"long-assert(4)",

	"n1-val(10)",   
	"n1-val(50)",
	"n1-val(100)",
	"n1-val(200)",
	"n1-val(500)",
	"n1-val(1000)",

	"mp(5)",
	"mp(6)",
	"mp(7)",
	"mp(8)",
	"mp(9)",    
	"mp(10)"

]

for b in synthetic_benchmarks:
    for m in ["verify", "rand", "3phstar"]:
        print(f"cd ~/michalis/genmc-tool/luan && ./table1n2.sh \"{b}\" out/synthetic/ 30 1800 {m}")