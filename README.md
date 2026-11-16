# SorterHunter
An evolutionary approach to find small and low latency sorting networks

## About
SorterHunter is a C++ program used to find well performing sorting networks.
For a number of input sizes, the method used by the program succeeded in reducing the upper bound of S(n) compared to previous work. (Definition: see "The Art of Computer Programming Vol. 3 - Sorting and Searching" - D.E. Knuth, 1998)

n  | S(n) previous upper bound | S(n) new upper bound
--  | ------------------------- | --------------------
18 | 78 (*) | 77
19 | 86 (*) | 85
20 | 92 (*) | 91
21 | 102 (*) | 99
22 | 108 (*) | 106
23 | 118 (*) | 114
24 | 123 (*) | 120
25 | 133 (*) | 130
26 | 140 (*) | 138
27 | 150 (*) | 147
28 | 156 (*) | 155
29 | 165 ($) | 164
33 | 201 ($) | 199
34 | 211 ($) | 209
35 | 223 ($) | 220 
36 | 231 ($) | 227
37 | 244 ($) | 240
38 | 253 ($) | 250
39 | 263 (^) | 259
40 | 269 (^) | 265
41 | 284 (^) | 282
42 | 293 (^) | 291
43 | 305 (^) | 303
44 | 313 (^) | 309
45 | 326 (^) | 324
46 | 334 (^) | 332
47 | 343 (^) | 340
48 | 349 (^) | 346
49 | 364 (^) | 365
53 | 411 (^) | 410
54 | 422 (^) | 421
55 | 433 (^) | 432
56 | 439 (^) | 438

(*) Ref: "Using Symmetry and Evolutionary Search to Minimize Sorting Networks", Valsalam&Miikkulainen, 2013.  
($) Compared to smallest Batcher odd-even merge network  
(^) Compared to Van Voorhis (4,4) merge strategy  


The results above were obtained while keeping also the number of parallel operation steps - a.k.a. "depth" - low. 

For 25 and 26 inputs, the program was able to reduce the optimal depth upper bound from 14 to 13 layers. A similar reduction to 15 layers was achieved for 34 inputs, and 17 layers for 42, 43 and 44 inputs.

I committed the program to the public domain as inspiration source for further improvements on the subject of sorting networks.  

The following lists of networks have been compiled for your convenience:
* the [list of best performing sorting networks](https://bertdobbelaere.github.io/sorting_networks.html) keeps track of the smallest and fastest networks with up to 32 inputs (as far as known by the author), together with the known bounds for depth and size.  
* the [extended list of sorting networks](https://bertdobbelaere.github.io/sorting_networks_extended.html) is a similar, larger table for up to 64 inputs.
* sorting networks of smallest known size optimized for [average swap count](https://bertdobbelaere.github.io/sorting_networks_avgswaps.html) that may give a performance boost on some target architectures.
* sorting networks of smallest known size optimized for [worst case swap count](https://bertdobbelaere.github.io/sorting_networks_maxswaps.html) regardless of the input.
* a [list of median selection networks](https://bertdobbelaere.github.io/median_networks.html) that were optimized using SorterHunter.


## The program
The program is very straightforward to build (just "make") on a Linux machine. Pass the configuration file as the first argument. Without a timeout, the search normally runs until interrupted, printing improvements as they are found. Current version is limited to 64 inputs.

```sh
./SorterHunter sample_config.txt               # run until interrupted
./SorterHunter sample_config.txt --timeout 30  # stop after approximately 30 seconds
```

`--timeout` accepts a positive whole number of wall-clock seconds (1 to 4294967295). The deadline starts before the configuration is loaded and includes prefix and test-vector preparation. On timeout the program exits normally with status 0, after printing a message to stderr; results already reported remain on stdout. The deadline is cooperative: a long library operation, memory allocation or blocked I/O can delay the exit beyond the requested time. A search may also finish or fail earlier for other reasons.

### Optional compiler tuning

The default build already uses `-O3`, LTO, native CPU tuning and compiler-specific optimization flags. On Linux, GCC or Clang builds can optionally use `-fno-plt`:

```sh
make clean
make NO_PLT=1
make test NO_PLT=1
```

This changes dynamic calls to use GOT indirection and resolves affected symbols at load time instead of lazy PLT binding. It does not disable symbol interposition, but may affect startup and tooling, and its performance benefit depends on the workload and platform. It is **off by default**: local measurements were mixed and did not establish a consistent improvement. Unsupported platforms or compilers reject `NO_PLT=1` with an error. `-march=native` already makes the binary specific to the build CPU; do not assume it will run on older machines.

For experiments, `make TUNE_FLAGS="..." EXTRA_LDFLAGS="..."` appends custom compiler and linker flags. Run `make clean` when changing flags, and benchmark and test on the deployment machine. Avoid flags that change language or math semantics unless those changes are intentional.

Profile-guided optimization is also available using an actual representative configuration as the training workload. For example, a bounded training run can be started with:

```sh
make pgo NO_PLT=1 PGO_RUN_ARGS="sample_config.txt --timeout 30"
```

Use a workload representative of production, since a profile for a different search mode may reduce performance. `make pgo` rebuilds the binary and discards existing local PGO profile files.

For the manual Clang PGO workflow, generate and train the instrumented binary
before applying its profile:

```sh
make pgo-gen CXX=clang++
./SorterHunter sample_config.txt --timeout 30
make pgo-use CXX=clang++
# Or use: make pgo-train CXX=clang++ PGO_RUN_ARGS="sample_config.txt --timeout 30"
```

`make pgo-use` converts `default.profraw` to `default.profdata` automatically
when needed and reports an error before cleaning the binary if neither profile
exists. The instrumented run must exit normally to write the raw profile;
`Ctrl+C` or a forced kill may not write it. Use `llvm-profdata` from the same
LLVM toolchain as Clang, or specify e.g. `LLVM_PROFDATA=llvm-profdata-18`.
If `LLVM_PROFILE_FILE` was set to a different path during a manual run, merge
that file explicitly into `default.profdata` with `llvm-profdata merge` first.
`make pgo-gen` rebuilds the instrumented executable but does not erase existing
profiles; remove old profiles or use `make pgo` for a fresh training run.


### Clang context-sensitive PGO and BOLT (optional)

The following workflows use a separate instrumented build for each profiling
stage. Install a compatible Clang and `llvm-profdata`; BOLT additionally needs
`llvm-bolt` **with its instrumentation runtime**, and a supported Linux ELF
CPU (normally x86-64 or AArch64). Use matching LLVM versions and put their
binaries on `PATH` (or set `CXX`, `LLVM_PROFDATA`, and `LLVM_BOLT`).

```sh
# Two-stage IR-PGO + context-sensitive profiling; output: ./SorterHunter
make cspgo PGO_RUN_ARGS="configs/webrtc/feedback_sort_8.txt --timeout 30"

# Ordinary Clang optimized build + instrumented BOLT; output: ./SorterHunter.bolt
make bolt PGO_RUN_ARGS="configs/webrtc/feedback_sort_8.txt --timeout 30"

# IR-PGO -> context-sensitive PGO -> BOLT; output: ./SorterHunter.bolt
make cspgo-bolt PGO_RUN_ARGS="configs/webrtc/feedback_sort_8.txt --timeout 30"
```

`cspgo` trains a `-fprofile-generate` build, rebuilds with its profile and
`-fcs-profile-generate`, then merges the base and context-sensitive profiles
before the final `-fprofile-use` build. It does not mix the distinct Clang
`-fprofile-instr-generate` format used by the older `make pgo` workflow.
BOLT rebuilds with `--emit-relocs`, instruments a copy and trains it without
requiring Linux perf/LBR privileges. It writes `SorterHunter.bolt` separately;
`SorterHunter` remains the un-BOLTed build. Rebuild/retrain after source, flags,
or workload changes. `make distclean` deletes the generated profiles and both
binaries; `make clean` deletes the binaries but keeps profiles.

Training runs must **exit normally** so their profiles are flushed: use
`--timeout` or a naturally terminating config, not `kill -9` or an external
hard timeout. The example only demonstrates the workflow; train on a workload
representative of the real use case. Check binary correctness and benchmark
both variants on the target hardware: CSPGO, BOLT, or extra tuning can also
make a particular workload slower. This repository does not claim a measured
BOLT speedup.

The existing Clang `-mllvm` inline and unroll thresholds remain configurable
with `LLVM_INLINE_THRESHOLD` and `LLVM_UNROLL_THRESHOLD`. When lld is used,
these are also forwarded to its LTO backend. Optional experimental backend
flags can be supplied via `LLVM_EXTRA_FLAGS` for compilation and
`LLVM_LTO_EXTRA_FLAGS` for lld; do not assume an undocumented LLVM option is
portable across versions. BOLT layout flags can be overridden with
`BOLT_FLAGS`. For example, run the tests against the optimized binary with
`python3 tests/timeout_test.py ./SorterHunter.bolt` and
`python3 tests/webrtc_configs_test.py ./SorterHunter.bolt`.


## Working principles
After the config file is read, the program works as follows:
#### Determining the prefix to use
During the entire run time of the (current) program, the same prefix is used. There are three options:
1. Empty prefix - all CEs (compare-exchange nodes) participate in the evolutionary optimisation
2. Fixed prefix - provided by the user in the config file
3. Greedy prefix - in a simple algorithm I call "greedy algorithm A", nodes are added one by one starting from an empty network and up to the specified prefix size is reached. For each step, a CE is randomly chosen among those that result in the minimum number of remaining (partially sorted) patterns after the prefix network (assuming application of the "*zero-one principle*"). I expect that still much can be improved on the prefix selection algorithm.
#### Preparing the test vectors
With an empty prefix, testing a network with N inputs by blind application of the "*zero-one principle*" would require 2^N input vectors of N elements to be sent through the candidate sorter. While CE nodes are added in the prefix, the set of possible output patterns of the prefix gradually decreases. To test the remainder of the network, only the possible output patterns of the prefix network need to be considered as input vectors. As the prefix is known before we will iteratively try to improve the network, the test vectors can be enumerated in a fixed list. Test vectors are placed in pseudorandom order. The reason for this is that in this way we increase the chances that an invalid candidate sorter (the vast majority!) will be rejected early in the test process, assuming not all test vectors are applied at once. Further, using 0's and 1's as input, the behaviour of a CE can simply be modelled as a combination of a single "and" and "or" gate. As the order of comparisons and exchanges for a sorting network is fixed, we use a bit-parallel approach to sort multiple test vectors in parallel (64 on a 64 bit machine), obtaining a considerable speed increase.
#### Determining an initial candidate sorter
Perhaps the weakest part of the algorithm today: the initial candidate is obtained by randomly adding CEs to the network until a valid sorter is obtained. Although there are many obvious ways to create a better initial network, it's hard to guarantee that a "good" initial network won't bias the solutions obtained through evolution. Work to do.
#### The never ending loop (that is: until Ctrl+C)
If you support the evolution theory, you also agree that it is a slow process. This program produces output in far less than a billion years, but it could easily run for that long.
The main iteration loop will take a copy of the latest accepted network and apply one or more mutations to it. Six mutations are currently part of the program, and their relative probabilities can be tuned via the config file.
The "regular" mutations are:
1. Removal of random CE from the variable (i.e. "postfix") list: "downhill" step
2. Swap two CEs at random positions in the list
3. Replace a CE at a random position with another random CE
4. Cross the I/O lines of two CEs at random positions in list
5. Swap neighbouring CEs that share a connection
6. Change 1 connection of a random CE

The mutations are accepted on condition that the mutant sorting network is valid. This is determined by applying the test vectors mentioned before.
An infrequent modification is the insertion of a duplicate or random CE at a random position in the list. The "uphill" change allows the algorithm to escape from local minima. 
#### Reporting results
Users of sorting networks are interested in low size, low depth but maybe also in trade-offs between them. To satisfy all, the program uses an *orthogonal convex hull* approach that keeps track of all (size,depth) pairs that are not outperformed by other networks.
