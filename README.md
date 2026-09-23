# FlagChase — Concurrent 2D Agent Simulation

<p align="center">
  <strong>C++17 concurrency, shared-state synchronization, benchmarking, and spatial occupancy optimization</strong>
</p>

<p align="center">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white">
  <img alt="Linux" src="https://img.shields.io/badge/Linux-tested-FCC624?logo=linux&logoColor=black">
  <img alt="CMake" src="https://img.shields.io/badge/CMake-3.15%2B-064F8C?logo=cmake&logoColor=white">
  <img alt="ThreadSanitizer" src="https://img.shields.io/badge/ThreadSanitizer-supported-success">
  <img alt="ASan + UBSan" src="https://img.shields.io/badge/ASan%20%2B%20UBSan-supported-success">
</p>

## Overview

**FlagChase** is a multithreaded 2D agent simulation written in C++17. The Looney Tunes-inspired flag-chase rules are the demonstration workload; the engineering focus is the concurrent simulation itself: one worker thread per agent, mutex-protected shared state, agent interactions, collision/occupancy checks, reproducible benchmarking, and performance optimization.

The current implementation uses a coarse-grained global board lock for correctness. A measured scalability bottleneck in the original occupancy check was then addressed by replacing an **O(N) scan across all agents** with a **direct spatial occupancy index** that maps each board cell to its current occupant.

### Highlights

- **Thread-per-agent execution** using `std::thread`
- **RAII-based synchronization** around shared simulation state
- Real agent behaviors: movement, jumping, burst movement, shooting, freezing, and cooldowns
- Debugged a real shared-state race involving an unsynchronized timer read
- **O(1) exact-cell occupancy lookup** using a discrete spatial index
- Reproducible benchmark and sweep modes with CSV output
- Streaming latency statistics using Welford aggregation
- Optional mutex wait-time instrumentation
- Dedicated **ThreadSanitizer** and **AddressSanitizer + UBSan** build modes
- Interactive ASCII rendering for normal runs; headless execution for benchmarks
- Optional **browser dashboard** (`--ui`) with a live canvas view, pause/resume/speed control, click-to-inspect agents, wall editing, and rolling throughput/frozen-agent charts

---

## Architecture

```text
                             main()
                               │
                    parse args / print config
                               │
            ┌──────────────────┼──────────────────┐
            │                  │                  │
       interactive         benchmark            sweep
            │                  │                  │
            └──────────────────┴──────────────────┘
                               │
                          runOneGame()
                               │
                         shared Board
                               │
                     ┌─────────┴─────────┐
                     │                   │
                  board.mtx         render_mtx
                     │                   │
          shared simulation state      stdout
                     │
         ┌───────────┼───────────┐
         │           │           │
      thread 0    thread 1    thread N
       agent 0     agent 1     agent N
```

Every active agent owns an OS thread. Those workers interact through a single shared `Board` instance containing positions, terrain, timers, cooldowns, step counters, render state, and the spatial occupancy index.

The current synchronization model intentionally favors **simple correctness** over maximum parallelism: mutable board state is accessed under `board.mtx`. This means the project has a clear tradeoff to analyze — many agent threads execute independently, but updates to shared board state serialize through one global critical section.

---

## Concurrency Model

### Shared state

The `Board` owns the primary mutable simulation state:

```text
toonPos[]        current agent positions
occupant[]       direct board-cell → agent index
frozen_until[]   per-agent freeze deadline
next_shot_ok[]   per-agent shooting cooldown
steps[]          per-agent movement count
cell[][]         static terrain / walls
grid[]           ASCII render buffer
```

`BoardLock` wraps `board.mtx` using RAII so lock release is tied to scope. `RenderLock` separately serializes event output.

### Lock ordering

The code enforces one lock-order rule:

```text
board.mtx  →  render_mtx
```

A thread-local assertion catches attempts to acquire the board lock while already holding the render lock in debug builds, reducing the chance of introducing an inverted lock-order deadlock later.

### Race-condition debugging

During development, a genuine race was found in the freeze-timer path: one code path read `frozen_until[t]` without synchronization while another path could update that same value while holding the board mutex.

The fix was to move the read under the same synchronization discipline as the write.

The important lesson is simple:

> Protecting writes alone is not sufficient when concurrent reads can race with those writes.

---

## Agent Behaviors

The three behavior roles repeat when the simulation is run with more than three agents:

| Role | Behavior |
|---|---|
| **RoadRunner** | Faster pacing with occasional burst movement |
| **Coyote** | Can jump over a blocked cell |
| **Yosemite Sam** | Can target another agent, shoot, and temporarily freeze it subject to a cooldown |

All behaviors operate against the same shared board state, making them useful for exercising synchronization under different access patterns.

---

## Spatial Occupancy Optimization

### Original approach

The original occupancy test scanned every agent whenever a worker wanted to determine whether a destination cell was occupied:

```cpp
for (size_t k = 0; k < board.toonPos.size(); ++k) {
    if (static_cast<int>(k) != self &&
        board.toonPos[k].r == r &&
        board.toonPos[k].c == c) {
        return true;
    }
}
```

That made each lookup **O(N)** in the number of agents.

Because the lookup happened while the global board mutex was held, increasing agent count had two effects at once:

```text
more agents
   ↓
longer occupancy scan
   ↓
longer critical section
   ↓
other workers wait longer for board.mtx
```

### Current approach

The board now maintains a flat occupancy index:

```cpp
vector<int> occupant;   // -1 = empty, otherwise agent id
```

A board coordinate maps directly to one slot:

```cpp
index = row * board.C + col;
```

Occupancy checking becomes a direct lookup:

```cpp
int who = board.occupant[index];
return who != -1 && who != self;
```

Since this simulation already uses a discrete grid and allows at most one agent per cell, a one-cell-to-one-occupant index is simpler and more precise than a generalized bucket structure.

### Complexity

| Operation | Before | Current |
|---|---:|---:|
| Exact-cell occupancy lookup | **O(N)** | **O(1)** |
| Position/index update | O(1) | **O(1)** |
| Additional memory | none | **O(rows × cols)** |

`move_toon_locked()` updates `toonPos` and the occupancy index together while the existing board lock is held. Debug assertions verify that both representations agree before each move.

---

## Performance

Benchmarks use the same `runOneGame()` simulation core as normal execution. Benchmark mode disables rendering and artificial delays, uses a fixed tick budget, runs repeated trials, and reports throughput plus inter-tick latency.

### Original O(N) occupancy baseline

400 × 1000 board, 10-trial sweep:

| Agents | Throughput | Latency |
|---:|---:|---:|
| 10 | 2457.6 ± 57.9 ticks/s | 407.1 ± 53.2 µs |
| 100 | 2469.2 ± 22.0 ticks/s | 405.0 ± 37.8 µs |
| 1,000 | 2434.6 ± 40.5 ticks/s | 410.8 ± 51.5 µs |
| 10,000 | 2193.3 ± 80.1 ticks/s | 456.5 ± 92.6 µs |
| 50,000 | **1676.8 ± 31.9 ticks/s** | **596.6 ± 103.7 µs** |

### After spatial indexing

At 50,000 agents, repeated measurements reached approximately:

- **2,200–2,500 ticks/sec**
- **400–455 µs latency**
- **31–49% higher throughput** than the original O(N) implementation

Low-agent-count changes were small, which is consistent with the optimization: the linear scan was inexpensive when `N` was small and became increasingly important at scale.

> Benchmark claims should always be reproduced on the same machine, build type, board dimensions, seed policy, and trial configuration before comparison.

---

## Benchmarking Methodology

The benchmark path is intentionally separate from interactive presentation concerns:

- rendering forced **off**
- `delay_ms` forced to **0**
- fixed tick budget via `--max-steps`
- multiple independent trials
- one seed per trial derived from the configured base seed
- throughput measured over a bounded trial window
- inter-tick latency accumulated using Welford's online algorithm
- aggregate results printed to stdout
- sweep results written to CSV

Example sweep:

```bash
./build/toons \
  --rows 400 \
  --cols 1000 \
  --seed 12345 \
  --max-steps 10000 \
  --benchmark-trials 10 \
  --sweep \
  --sweep-counts 10,100,1000,10000,50000 \
  --csv-out bench_spatial_index.csv \
  --no-render
```

---

## Web Dashboard

An optional browser dashboard replaces ASCII rendering with a live, interactive view of the simulation, served from an embedded HTTP server that runs alongside the worker threads:

```bash
./build/toons --rows 40 --cols 80 --toons 30 --ui --ui-port 8080
```

Then open `http://localhost:8080`. The dashboard:

- renders the board and agents on a pannable, zoomable canvas
- supports pause / resume / stop and live agent-delay (speed) control
- lets you click an agent (or search by ID) to inspect its position, state, freeze/cooldown timers, and step count, including its direct `occupant[]` index
- supports click-to-add/remove walls
- plots rolling throughput and frozen-agent count

The server exposes three endpoints polled by the dashboard's own JS, and usable directly:

```text
GET  /api/layout            board dimensions, flag position, walls
GET  /api/state?selected=N  live agent snapshot, optionally with one agent's detail
POST /api/control?cmd=...   pause | resume | stop | speed | freeze | unfreeze | addwall | removewall
```

The browser only ever reads a snapshot of the shared `Board` (via the same `board.mtx`-protected accessors used by the simulation loop) and queues commands into it — it never mutates simulation state directly, keeping the visualization layer separate from the synchronization-critical path. `--ui` disables ASCII rendering for the run; it is not meant for `--benchmark`/`--sweep`, which stay headless.

---

## Build

### Requirements

- C++17-compatible compiler
- CMake 3.15+
- Linux or another environment with C++ threading support

### Release build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Run the simulation:

```bash
./build/toons
```

Example custom run:

```bash
./build/toons \
  --rows 30 \
  --cols 80 \
  --toons 12 \
  --seed 12345 \
  --max-steps 5000
```

---

## Sanitizers

Sanitizer builds are intentionally separate from performance builds because instrumentation changes runtime characteristics.

### ThreadSanitizer

```bash
cmake -S . -B build-tsan \
  -DSANITIZER=thread \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build-tsan -j"$(nproc)"
```

Example stress run:

```bash
./build-tsan/toons \
  --rows 400 \
  --cols 1000 \
  --toons 1000 \
  --benchmark \
  --benchmark-trials 5 \
  --max-steps 10000 \
  --seed 12345 \
  --no-render
```

### AddressSanitizer + UBSan

```bash
cmake -S . -B build-asan \
  -DSANITIZER=address \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build-asan -j"$(nproc)"
```

```bash
./build-asan/toons \
  --rows 400 \
  --cols 1000 \
  --toons 1000 \
  --benchmark \
  --benchmark-trials 5 \
  --max-steps 10000 \
  --seed 12345 \
  --no-render
```

---

## Lock Instrumentation

The project can optionally measure time spent waiting for `board.mtx`.

Build separately so timing instrumentation does not contaminate the primary throughput measurements:

```bash
cmake -S . -B build-instrumented \
  -DLOCK_INSTRUMENTATION=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-instrumented -j"$(nproc)"
```

This mode reports the average percentage of each worker thread's lifetime spent waiting to acquire the global board mutex.

---

## Useful CLI Options

```text
--rows N
--cols N
--toons N
--max-steps N
--seed N
--delay-ms N
--shoot-chance X
--shoot-cooldown N
--freeze-ms N
--jump-chance X
--no-render
--ui
--ui-port N
--ui-max-agents N
--benchmark
--benchmark-trials N
--sweep
--sweep-counts LIST
--csv-out PATH
```

See all options:

```bash
./build/toons --help
```

---

## Current Limitations

The current implementation intentionally keeps several design constraints visible rather than hiding them:

- A **single global board mutex** serializes mutable board-state access.
- The simulation uses **one OS thread per agent**, so very large agent counts can oversubscribe the machine.
- Interactive ASCII rendering is synchronous and not designed for high-scale runs; the `--ui` browser dashboard is a better fit for large agent counts but still polls state rather than pushing updates.
- The source is currently concentrated in `src/main.cpp` rather than split into separate engine, UI, and domain modules.
- The occupancy index optimizes collision lookup, but it does not remove the global-lock architecture itself.

These constraints are useful because they make the tradeoffs measurable and provide clear directions for future work.

---

## Roadmap

Planned improvements are intentionally kept separate from the benchmarked core so each architectural change can be measured independently.

### GUI / simulation dashboard

The browser dashboard described above (`--ui`) covers most of what was originally planned here: a live 2D view, pause/resume, speed control, click-to-inspect agents with live position/freeze/cooldown/step display, the spatial-occupancy index overlay, wall editing, and rolling throughput/frozen-agent charts. Not yet implemented:

- a **reset** control (only pause / resume / stop)
- changing **agent count or board size** without restarting the process
- a general environment editor beyond wall add/remove

### Additional architectural experiments

Potential future work includes:

- separating simulation, rendering, and benchmark code into modules
- reducing the scope of the global critical section
- comparing alternative scheduling models after establishing reproducible baselines

Those are deliberately future experiments rather than claims about the current implementation.

---

## Why This Project Exists

The project started as a small concurrent simulation and became an exercise in systems reasoning:

```text
build concurrent behavior
        ↓
find a real synchronization bug
        ↓
make shared-state access consistent
        ↓
measure scaling behavior
        ↓
identify an O(N) hot path inside the critical section
        ↓
change the data representation
        ↓
rerun the same benchmark
        ↓
quantify the improvement
```

The main goal is not the game itself. It is understanding how correctness, synchronization, algorithms, and measurement interact in a concurrent C++ program.
