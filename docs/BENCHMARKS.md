# Concurrency benchmarks: coarse global-mutex baseline

This documents the `--benchmark`/`--sweep` CLI modes, exactly how the numbers
in the README's Concurrency Invariants section were produced, and confirms a
clean run. It measures the **current** single-global-lock design (`board.mtx`
— see README) as a baseline. **It does not change the architecture.** A later
task compares this baseline against finer-grained locking / lock-free
strategies; this document and `bench/bench_global_mutex_baseline.csv` are
that comparison's control group, not a one-off thrown away after this task.

## What `--benchmark` and `--sweep` measure

`--benchmark` runs `--benchmark-trials` (default 10) independent trials at
the configured `--toons` agent count. Each trial:

* builds a fresh `Board` and spawns `--toons` worker threads (the exact same
  `runOneGame` simulation core used by normal play — not a separate,
  simplified benchmark harness, so the measured lock-acquisition pattern is
  the real one, not a stand-in),
* forces `render=off` and `delay-ms=0` internally regardless of what was
  passed (headless, no artificial pacing — you're measuring the engine, not
  the visual pacing sleep),
* **disables the "reach the flag" win condition** for the trial's duration:
  every trial runs the full `--max-steps` tick budget, so every trial (and
  every agent count in a sweep) does the same fixed amount of work and is
  directly comparable. (In normal play, win-to-end-game stays exactly as
  before — this only applies under `--benchmark`.)

Two things are reported, both computed only from ticks/samples that fall
strictly inside a `[trialStart, trialEnd]` measurement window (a `measuring`
flag brackets it — see "Measurement pitfalls" below for why that mattered):

* **Throughput** — ticks (successful agent moves) per second, mean ± stddev
  *across* the N trials.
* **Frame latency** — wall-clock time between consecutive ticks (any agent,
  system-wide), microseconds. Every `++totalSteps` already happens while
  `board.mtx` is held, so timestamping it there needs no extra
  synchronization. All trials' samples are pooled into one mean/stddev via
  Welford's parallel-combine formula, not stored individually.

`--sweep` runs `--benchmark` across a comma-separated list of agent counts
(`--sweep-counts`, default `10,100,1000,10000,100000`) and writes one row per
agent count to `--csv-out` (default `bench_global_mutex_baseline.csv`).

## Exact commands used

```bash
# Build (plain Release — no sanitizers, no instrumentation: this is the
# build the real throughput/latency numbers come from)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Sweep: fixed board across every tier so agent count is the only variable
./build/toons --sweep --sweep-counts "10,100,1000,10000,50000" \
  --benchmark-trials 10 --rows 400 --cols 1000 --max-steps 10000 --seed 1 \
  --csv-out bench/bench_global_mutex_baseline.csv
```

Board size (400×1000 = 400,000 cells) is fixed across every tier — only
`--toons` varies — so the sweep isolates agent count as the sole independent
variable. It's sized to comfortably hold 50,000 agents (~12.5% density
after walls) without the position-placement rejection sampler falling back
to its linear scan.

### Why 50,000, not 100,000

The task asked for a tier one order of magnitude past 10,000. `100,000`
`std::thread`s could not be created in this environment:

```
$ ./build/toons --benchmark --toons 100000 ...
terminate called after throwing an instance of 'std::system_error'
  what():  Resource temporarily unavailable
```

`dmesg` confirms the cause:

```
cgroup: fork rejected by pids controller in /user.slice/user-1000.slice/session-2.scope
```

`/sys/fs/cgroup/user.slice/user-1000.slice/pids.max` is `83199` for this
entire desktop session (already at ~760 baseline threads from other running
software) — a hard ceiling, not a `ulimit` this process controls, and not
something raised for this task (it's session-wide, would need root, and
changing it wasn't asked for). `50,000` was verified to run reliably with
comfortable margin under that ceiling and is the tier actually swept.

### Lock-wait-time measurement (separate build, not mixed into the sweep)

```bash
cmake -S . -B build-instrumented -DLOCK_INSTRUMENTATION=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-instrumented -j$(nproc)

./build-instrumented/toons --benchmark --benchmark-trials 5 --toons 1000  --rows 400 --cols 1000 --max-steps 10000 --seed 1
./build-instrumented/toons --benchmark --benchmark-trials 5 --toons 10000 --rows 400 --cols 1000 --max-steps 10000 --seed 1
```

`LOCK_INSTRUMENTATION` times every `board.mtx` acquisition/release inside
`BoardLock` (see `src/main.cpp`). That overhead would skew the real
throughput/latency figures, so it's a separate CMake option producing a
separate binary — never combined with the sweep build, and never with
`-DSANITIZER`.

### `perf` was not available

`perf lock` was the first choice per the task, with the custom
instrumentation above as the stated fallback. `perf` is not installed in
this environment, and `/proc/sys/kernel/perf_event_paranoid` is `3` (would
block unprivileged perf counter access even if it were installed — fixing
that needs root). Rather than install a system package or touch a kernel
parameter unasked, the fallback was used as the primary (and only) method.
`perf stat -e cache-misses` (the task's secondary confirmation check) is
skipped for the same reason — noted here rather than fabricated.

## Measurement pitfalls found and fixed while building this

Two real bugs were caught and fixed during development, before any numbers
below were collected for real (not after — these aren't caveats on the
final numbers, they're why the final numbers are trustworthy):

1. **Tick count read after `join()`, not at `trialEnd`.** Worker threads
   only re-check `gameOver` at the top of their loop, so once the polling
   thread detects the tick budget was hit and flips `gameOver`, up to
   ~`toons` threads can each complete one more in-flight tick before they
   next check and exit. Originally, `res.ticks` was `totalSteps.load()`
   taken *after* `join()` — i.e. after that whole drain completed — while
   the elapsed-time measurement stopped at the *earlier* `trialEnd`. At
   `toons=10000` this produced `n=19548` recorded ticks against a
   configured budget of `10000`: throughput was silently inflated by
   roughly 2x, worst exactly at the high-agent-count end where accurate
   numbers mattered most. Fixed by snapshotting the tick counter at the
   same instant as `trialEnd` (before `join`), and by gating the stopping
   condition on ticks produced *since* `trialStart` rather than the raw
   counter (so ticks produced during thread spawn, before measurement
   starts, can't shorten the measured window either).
2. **Lock-wait percentage computed against the wrong time base.** The
   per-thread `board.mtx` wait-time counter (`tlBoardWaitNs`) starts
   accumulating the moment a thread is created — before the main thread
   finishes spawning the rest and starts the trial's timer — so dividing it
   by the trial's `elapsedSec` produced nonsensical values above 100% (110%
   and 196% were observed at 1,000 and 10,000 agents on the first attempt).
   Fixed by measuring each thread's wait time against *its own*
   creation-to-exit lifetime instead of the trial's window — numerator and
   denominator now span the same interval by construction, so the result is
   always a valid percentage in [0, 100].

Both were caught by noticing implausible output (an exact ~2x tick overshoot;
a >100% percentage) before writing up any conclusions, not discovered later.

## Results

See `bench/bench_global_mutex_baseline.csv` for the raw sweep data and
`docs/bench_global_mutex_baseline.png` for the plot. Summary:

| Agents | Throughput (ticks/sec) | Latency (µs) | Lock wait |
|---|---|---|---|
| 10 | 2457.6 ± 57.9 | 407.1 ± 53.2 | — |
| 100 | 2469.2 ± 22.0 | 405.0 ± 37.8 | — |
| 1,000 | 2434.6 ± 40.5 | 410.8 ± 51.5 | 99.9% |
| 10,000 | 2193.3 ± 80.1 | 456.5 ± 92.6 | 100.0% |
| 50,000 | 1676.8 ± 31.9 | 596.6 ± 103.7 | — |

(Lock-wait percentages are from a separate 5-trial instrumented run, not
the 10-trial sweep — see above for why they're kept apart from the CSV's
throughput/latency columns, which come from the uninstrumented build.)

Throughput is flat within noise from 10 to 1,000 agents (2434–2469
ticks/sec, a 1.4% spread — consistent with run-to-run scheduling noise, not
a trend), then degrades monotonically: −11.2% at 10,000 agents and −32.1%
at 50,000 agents relative to the 100-agent peak. Latency mirrors this: flat
around 405–411µs through 1,000 agents, then +12.1% at 10,000 and +46.6% at
50,000. Lock-wait time is already effectively 100% at 1,000 agents — with
`--delay-ms` forced to 0 for benchmarking, every worker thread immediately
re-attempts `board.mtx` the instant it releases it, so almost the entirety
of every thread's life is spent either holding or waiting for the one lock.

**This is the textbook signature of single-global-lock serialization, and
the numbers directly confirm it, not just the curve shape.** More agents
past roughly 1,000 don't buy more throughput on this codebase — they buy
more threads contending for the same one mutex, which is a wash at best and
a throughput *loss* past 10,000. The lock-wait measurement is the direct
confirmation: contention isn't inferred from the throughput curve bending
down, it's measured straight — at 1,000+ agents, threads are blocked on
`board.mtx` essentially 100% of their wall-clock lifetime.

### Cache-misses (secondary check, not run)

`perf stat -e cache-misses` was the task's secondary confirmation check —
skipped, see "`perf` was not available" above. Worth noting for context
regardless: with nearly every shared-state access already serialized behind
one lock, essentially all false-sharing candidates already live *inside*
that lock's critical sections. A cache-misses number, had it been
measurable, would have been confirmatory at best (contended cache lines
inside an already-contended lock), not a competing explanation for the
throughput/latency trend above — the lock-wait measurement already
directly accounts for it. No `alignas(64)` or similar cache-line fix was
applied, speculatively or otherwise: nothing in the data collected here
points at false sharing on something accessed concurrently *outside* the
lock, which is the one scenario that would justify it.

## Reproducing this run

```bash
rm -rf build build-instrumented
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
cmake -S . -B build-instrumented -DLOCK_INSTRUMENTATION=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build-instrumented -j$(nproc)

./build/toons --sweep --sweep-counts "10,100,1000,10000,50000" --benchmark-trials 10 \
  --rows 400 --cols 1000 --max-steps 10000 --seed 1 \
  --csv-out bench/bench_global_mutex_baseline.csv

./build-instrumented/toons --benchmark --benchmark-trials 5 --toons 1000  --rows 400 --cols 1000 --max-steps 10000 --seed 1
./build-instrumented/toons --benchmark --benchmark-trials 5 --toons 10000 --rows 400 --cols 1000 --max-steps 10000 --seed 1
```

The exact ceiling for `--toons` in `--benchmark`/`--sweep` mode depends on
the machine's `pids.max` cgroup limit (check
`/sys/fs/cgroup/**/pids.max` for the session's cgroup) — 100,000 may well
work on a machine without this constraint; nothing in the code caps it.
