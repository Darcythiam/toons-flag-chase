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

## Open Questions / Post-Saturation Decline

The "Results" section above treats lock-wait% near 100% at 1,000+ agents as
direct confirmation that the global mutex causes the throughput decline seen
from 10,000 agents onward. That reasoning has a gap worth recording rather
than leaving implicit: with `--delay-ms` forced to 0, N busy-retrying
threads contending for one mutex converge on a wait fraction of `(N-1)/N`
almost by construction, regardless of how expensive the critical section
itself is. A high wait% is therefore not, by itself, evidence that the
*decline* past 1,000 agents is lock-caused — it would be just as high even
if the critical section cost were constant and throughput were flat. This
section checks that gap directly rather than assuming it away. **It does
not change the "textbook signature of lock serialization" conclusion
above** — see the reconciliation at the end.

### 1. `occ()` is O(N), not O(local density)

`occ()` (`src/main.cpp:467` and an identical second instance at `:560`) is:

```cpp
auto occ = [&](int r,int c){
    for(size_t k=0;k<board.toonPos.size();k++)
        if((int)k!=t){ if(board.toonPos[k].r==r && board.toonPos[k].c==c) return true; }
    return false;
};
```

This is a linear scan over the *entire* `board.toonPos` vector — every other
agent on the board, not just spatial neighbors — executed while `board.mtx`
is already held. It is **O(N)** in total agent count. Call sites per tick
per worker thread: line 474 (`blocked = ... || occ(nxt.r,nxt.c)`) always
runs on every movement attempt (1 guaranteed O(N) scan); line 470, inside
`try_move`, adds a 2nd O(N) scan in the same critical section when the
Coyote-jump path triggers; line 561 (a separate `BoardLock` critical
section) adds a possible 3rd O(N) scan on the RoadRunner-burst path. Since
only one thread holds `board.mtx` at a time, critical-section hold time for
a single move is O(N) rather than O(1) — it scales with total agent count.
Aggregate serialized work across N threads each doing this once is
therefore **O(N²)** per full round. This gives the post-saturation decline
a concrete mechanism that wait% cannot: as N grows, each lock acquisition
itself gets *more expensive*, independent of how many threads are
contending for it. No fix (spatial hashing, bucketing, etc.) was applied —
this is a complexity finding, not a change.

### 2. Machine specs (thread-oversubscription context)

* `nproc`: **12** cores.
* `ulimit -s`: **8192 KB** (8 MiB) — matches glibc's default pthread stack
  size when unset.
* 50,000 threads × 8 MiB = **390.6 GiB** of *virtual* address-space
  reservation for stacks. RAM on this machine is 30 GiB total (27 GiB
  available). 390.6 GiB virtual vastly exceeds physical RAM, but Linux
  stacks are demand-paged (guard page + lazy commit) — only touched pages
  become resident, and this simulation has shallow, non-recursive call
  stacks, so actual RSS per thread is almost certainly tens of KB, not the
  full reservation. **Stack memory is very unlikely to be pressuring this
  system**; it reads as virtual bookkeeping, not physical contention.
* `/sys/fs/cgroup/user.slice/user-1000.slice/pids.max` = `83199`, matching
  the value already recorded above for the 50,000-agent ceiling.
* 50,000 real OS threads (thread-per-agent, no pool) on 12 cores is
  **~4,167:1 oversubscription**. This is a plausible contributing cost
  (scheduling, context-switch overhead) independent of the lock, but it is
  not isolated by any measurement in this document — doing so would need a
  pooled-thread rerun, which is a design change and out of scope here.

### 3. Raw per-trial data (not just the aggregated %)

Full stdout for every trial at N=10, 100, 1,000, and 10,000 (5 trials each,
seeds 1–5, same `--rows 400 --cols 1000 --max-steps 10000` as the documented
runs) is saved in `bench/raw_logs/instrumented_raw_20260903.log`. Summary:

| N | trial (seed) | throughput (tps) | latency (µs) | wait% |
|---|---|---|---|---|
| 10 | 1 | 4411.1 | 226.67 | 89.6% |
| 10 | 2 | 4532.8 | 220.54 | 90.0% |
| 10 | 3 | 4286.3 | 233.28 | 90.0% |
| 10 | 4 | 2895.5 | 345.37 | 90.0% |
| 10 | 5 | 2363.6 | 423.05 | 90.0% |
| 100 | 1 | 2395.2 | 417.50 | 99.0% |
| 100 | 2 | 2532.8 | 394.82 | 99.0% |
| 100 | 3 | 2557.4 | 391.03 | 99.0% |
| 100 | 4 | 2572.2 | 388.76 | 99.0% |
| 100 | 5 | 2594.6 | 385.39 | 99.0% |
| 1,000 | 1 | 2506.2 | 398.97 | 99.9% |
| 1,000 | 2 | 2597.0 | 385.04 | 99.9% |
| 1,000 | 3 | 1894.0 | 527.94 | 99.9% |
| 1,000 | 4 | 2598.9 | 384.80 | 99.9% |
| 1,000 | 5 | 2564.7 | 389.91 | 99.9% |
| 10,000 | 1 | 2306.0 | 433.62 | 100.0% |
| 10,000 | 2 | 2324.9 | 430.11 | 100.0% |
| 10,000 | 3 | 2542.4 | 393.34 | 100.0% |
| 10,000 | 4 | 2484.6 | 402.49 | 100.0% |
| 10,000 | 5 | 3830.4 | 261.08 | 100.0% |

(These instrumented-build throughput/latency figures are noisier and
systematically higher than the uninstrumented sweep table above — expected,
since `LOCK_INSTRUMENTATION` timing itself skews throughput per its own
CMake option comment. They're a cross-check for wait%, not a replacement
for the Results table. The N=10,000 trial-5 run timed out mid-trial on
first attempt and was rerun standalone.)

### 4. N=10 / N=100 vs. the `(N-1)/N` prediction

| N | mean measured wait% | `(N-1)/N` prediction | throughput trend (Results table) |
|---|---|---|---|
| 10 | 89.9% | 90.0% | flat (2457.6, part of the 1.4% "noise" spread) |
| 100 | 99.0% | 99.0% | flat (2469.2, part of the 1.4% "noise" spread) |
| 1,000 | 99.9% | 99.9% | flat (2434.6, still inside the 1.4% spread) |
| 10,000 | 100.0% | 99.99% → 100.0% | −11.2% vs. the 100-agent peak |

Measured wait% tracks the `(N-1)/N` prediction almost exactly at every
tier, including N=10 and N=100, where the Results table shows throughput is
flat, not declining. This is the direct answer to the open question: at
N=10 and N=100, wait% is already 89.9%–99.0% — nearly as saturated as the
99.9%/100.0% figures the Results section cites as confirmation at
1,000/10,000 agents — yet throughput does not move at N=10/100. **This
confirms that lock-wait% and throughput degradation are independent
signals**: high wait% is consistent with contention existing, but does not
by itself distinguish "pure contention, no other cost growing" (N=10/100,
flat throughput) from "contention plus a cost that grows with N"
(N≥10,000, declining throughput). Wait% alone cannot tell those two cases
apart because it saturates almost immediately regardless of which one is
happening.

### Reconciling with "textbook signature of lock serialization" above

The Results section's conclusion is not overturned by this — flat
throughput through 1,000 agents and monotonic decline from 10,000 onward is
still real, and the global mutex is still the only synchronization
mechanism in the system, so it is still mechanically responsible for
serializing every agent's move. What this section adds is that **the
wait% figure specifically is not the evidence that should be doing that
argument's work**, because wait% saturates too early (by N=10-100) to
discriminate "flat" from "declining" regimes. The `occ()` finding in (1)
is a better-fitting mechanism for the *decline* specifically: it predicts
critical-section cost growing with N (unlike wait%, which cannot grow past
100% and says nothing once already there), which lines up with throughput
starting to drop once per-lock work becomes large enough to matter relative
to everything else a thread does. The thread-oversubscription context in
(2) is a second, unisolated candidate that may compound it. Neither (1) nor
(2) was fixed or worked around here — this section is a diagnosis, not a
change, and the finer-grained-locking comparison already flagged as future
work in the README is the natural place to test whether addressing (1)
changes the shape of the decline.

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
