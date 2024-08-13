# TPAw0v
The implementation presented in [Timely Progress Integrity: Low-overhead Online Assessment of Timely Progress as a Commodity](https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.ECRTS.2023.13)

### Recommanded: Start with the csc directory
The program `start_mp` in `csc`, upon executing, configures the infrastructure necessary, runs a target program `/bin/ls`, traces the target program, and prints out the trace data upon exiting. 

**TL;DR**
The purpose of `csc` is to provide a starting point for researches who want to dive deep into the CoreSight trace infrastructure. The code has been refactored a number of times for simplicity. `csc` does not entirely replicate the configuration presented in the paper. However, it has already performed one of the important aspect: set up the CoreSight infrastructure, and perform a toy example in tracing. It is intended to serve as a Hello World! for CoreSight ETM trace on ZCU102 or Kria board. The program in `csc` assumes the execution environment is some Linux on ZCU102 or Kria boards. 

What does the program `start_mp` in `csc` do in details? It first configures CoreSight components, so that they are ready to accept trace data from ETM. Most importantly, it only uses one Trace Memory Controller (TMC) *TMC1* (a.k.a *ETF1*) in Software FIFO mode. This is different from paper implementation in which all three *TMCx* are used to route the trace data to some memory storage. The `poller` as a piece of software can run anywhere on the platform and it constantly polls the RAM Read Data register on TMC to fetch trace data while the target program is running. You can even add flush logic to the `poller` so that the data is fetched in a real-time manner. The `poller` can be placed on other Processing Element (PE), e.g. other spare Cortex-A53, Cortex-R5, or FPGA. Then `start_mp` enables *ETM* to generate trace while fork the target program `/bin/ls` to be traced. Specifically, it sets a filter so that only the CPU activity for the target program within virtual address `0x400000 - 0x500000` will be traced. When the target program is finished, `start_mp` will disable the *ETM* and instruct `poller` to print out the trace data. 

### Running in the target
```shell
./start_mp
./deformat [the_number_of_active_ETMs]
./ctrace trc_0.out > trc_0.hum
```