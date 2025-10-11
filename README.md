# weazer

This repository contains the implementation of Weazer and some useful scripts for its evaluation. We implemented both Weazer and a baseline naive random tester, Random, on top of GenMC. These three tools are involved in our evaluation. The implementation can be found in `genmc-tool` folder. Please follow the `genmc-tool/README.md` instructions to build the tool. 

If building is successful, there will be a `genmc-tool/genmc` executable file. We use command line options to run different modes (weazer/random/verification). The scripts for evaluation are provided in `genmc-tool/luan/`. Running the scripts there, the output data will be dumped to `genmc-tool/luan/out`. We also provide some scripts to process the data and generate latex tables and figures. Please have your latex environment installed in order to render `genmc-tool/luan/main.tex` into pdf format. 


The evaluation consists of three parts: bug detection, coverage and different weazer heuristics. More detailed instructions can be found in `genmc-tool/luan/ReadMe.md`. 

- bug detection: We compare weazer with random and genmc. please see `table1.sh`, `table2.sh`, `table3.sh` and `figure1.sh` under `genmc-tool/luan/` for more details.

- coverage: We compare weazer with random. please see `genmc-tool/luan/figure2.sh` for more details. 

- heuristics: We compare three different settings of weazer on bug detection performance. please see `genmc-tool/table4.sh` for more details.


We thank you for your interest in our work. Any thoughts and suggestions would be highly appreciated.