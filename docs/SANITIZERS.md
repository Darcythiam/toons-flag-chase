# Sanitizer builds

Two opt-in CMake configurations exist alongside the default (untouched)
release build, selected with `-DSANITIZER=`:

| `SANITIZER` value | Flags applied |
|---|---|
| `none` (default) | none — identical to the original release build |
| `thread` | `-fsanitize=thread -g -O1 -pthread` |
| `address` | `-fsanitize=address,undefined -g -pthread` |

## Build commands

```bash
# ThreadSanitizer
cmake -S . -B build-tsan -DSANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j$(nproc)

# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DSANITIZER=address -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j$(nproc)

# Default release build is unaffected
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## CLI options added for stress testing

* `--toons N` — no longer capped at 3. Agent identity/ability *role* cycles
  every 3 indices (`t % 3` → RoadRunner/Coyote/YosemiteSam), so `--toons 4000`
  still exercises the shoot/freeze, jump, and burst code paths at scale
  instead of spawning 3997 idle agents.
* `--max-steps N` is now actually enforced. Previously it was parsed into
  `Options` and never read again anywhere in `main()` — the simulation had
  no way to stop other than a winner reaching the flag or Ctrl+C, so a
  literal "run N ticks" stress request was not achievable. The main wait
  loop now also exits once the shared step counter reaches `--max-steps`,
  which then releases all worker threads via `gameOver`.
* `--no-render` skips per-frame board/event printing (and the pacing sleep
  tied to it), so a stress run isn't bottlenecked on stdout I/O. The final
  one-line summary (winner, total steps) still prints.

## Stress methodology

The game's own win condition — first toon to reach the flag ends the run —
works against a literal "500 agents / 100k ticks in one run" target: the
board's global mutex serializes all state access, so wall-clock progress is
dominated by lock contention rather than agent count, and the *minimum*
starting distance to the flag among N independently-placed agents shrinks
roughly as `board_width / N`. At high agent counts, some agent reliably
starts near the flag and wins within a few dozen shared steps — increasing
`--toons` further doesn't buy more ticks, it just buys more concurrent
threads racing to lose almost immediately. Attempting to force literal
100,000-step runs by widening the board to compensate turns out impractical
(the board width needed scales with agent count, and TSan's per-lock
overhead means even a 50-agent / 6000-column sustained run only reaches
~2,200 total steps in 30s — see below).

So two complementary stress configurations were used instead, run under
each sanitizer.

Note: total step counts are **not** exactly reproducible run-to-run even
for the same `--seed`. `--seed` fixes the RNG stream, but which of the
racing threads actually gets scheduled before `gameOver` flips true is an
OS scheduling outcome, not a seeded one — under a globally-mutex-serialized
sim where the winner can need as few as 1–3 of its own steps, that's the
dominant source of variance. Rerunning the same seed/config twice back to
back produced 4,571 and (a separately reported) 27 total steps for the
same seed on one config, and 1,683 vs. 121 on another — both legitimate,
neither a bug. Treat the step counts below as "this order of magnitude was
reached," not as exact expected values a future run should match.

**A. High-concurrency / thread-churn** (5 seeds each): maximizes the number
of concurrent threads and the frequency of ability triggers (shoot/freeze/
jump), and repeats the full thread create → run → join lifecycle many
times. This specifically targets teardown-time bugs (a thread still
referencing state that's about to be destroyed), since each seed is a full
process lifecycle, not one long-lived race.

```bash
./toons --toons 4000 --rows 140 --cols 280 --max-steps 100000 \
  --shoot-chance 0.5 --jump-chance 0.5 --delay-ms 0 --no-render --seed <1..5>
```

Each run finished in 1–24s (varies with how close the nearest starting
agent happens to land to the flag) and hit the ability code paths (`Winner:
YosemiteSam` / `Coyote` / `RoadRunner` across the 5 seeds confirms all
three roles' logic executed and could win).

**B. Sustained duration** (5 seeds each): fewer agents (50) but a much
wider board (6000 columns) and a short shoot cooldown/freeze duration, so
the race runs for hundreds to thousands of shared steps instead of dozens,
sustaining lock contention and repeated freeze/cooldown state transitions
over a longer window per run.

```bash
./toons --toons 50 --rows 30 --cols 6000 --max-steps 100000 \
  --shoot-chance 0.5 --jump-chance 0.5 --shoot-cooldown 50 --freeze-ms 50 \
  --delay-ms 0 --no-render --seed <1..5>
```

Reached 15–2,213 total steps per run (TSan seed 1: 2,213 steps in ~23s;
under the ~10x TSan slowdown this is expected — the board mutex is a hard
serialization point, so the sim is effectively single-threaded throughput-
wise regardless of agent count).

## Results

All 20 runs (5 seeds × 2 configurations × 2 sanitizer builds) completed
with zero sanitizer reports:

```
$ for seed in 1 2 3 4 5; do ./build-tsan/toons --toons 4000 --rows 140 --cols 280 \
    --max-steps 100000 --shoot-chance 0.5 --jump-chance 0.5 --delay-ms 0 \
    --no-render --seed $seed; done 2>&1 | grep -c "WARNING: ThreadSanitizer"
0

$ for seed in 1 2 3 4 5; do ./build-asan/toons --toons 4000 --rows 140 --cols 280 \
    --max-steps 100000 --shoot-chance 0.5 --jump-chance 0.5 --delay-ms 0 \
    --no-render --seed $seed; done 2>&1 | grep -Ec "ERROR: AddressSanitizer|runtime error"
0
```

(same for the 50-agent/6000-column sustained configuration, both sanitizers)

## Bugs found and fixed

### 1. Data race on `frozen_until[t]` (TSan-confirmed)

`src/main.cpp`, worker loop: the "am I frozen" check read
`board.frozen_until[t]` with no lock held, while YosemiteSam's shot handler
writes `board.frozen_until[target]` under `board.mtx`. TSan reported this
directly (reproduced independently against the pre-fix code: 3 of 10 runs
at `--shoot-chance 0.6 --jump-chance 0.5 --delay-ms 0` with the default
3-toon board hit it — see report below). Fixed by taking `board.mtx` for
the read too, matching how every other access to `frozen_until` is guarded.

```
WARNING: ThreadSanitizer: data race
  Write of size 8 ... main.cpp:261 (frozen_until[target] = ..., under board.mtx)
  Previous read of size 8 ... main.cpp:194 (now() < board.frozen_until[t], unguarded)
```

### 2. Detached cooldown thread dangling into a destroyed stack frame

YosemiteSam's shoot cooldown was implemented as an atomic `bool sam_on_cd`
(a local variable in `main`) plus a **detached** thread per shot:

```cpp
sam_on_cd.store(true);
std::thread([&]{
    std::this_thread::sleep_for(std::chrono::milliseconds(opt.sam_cooldown_ms));
    sam_on_cd.store(false);
}).detach();
```

That lambda captures `sam_on_cd` by reference. `main()` joins all *worker*
threads before returning, but never joins or waits on these detached
timer threads. If one is still sleeping when `main()` returns, `sam_on_cd`
(and the rest of `main`'s stack frame) is destroyed while the detached
thread later tries to write to it — a stack-use-after-scope, and also a
single cooldown flag incorrectly shared across every YosemiteSam-role
agent once `--toons` allows more than one.

This is genuine undefined behavior by the standard (the referenced object's
lifetime has ended), and it's the reason `--toons` was capped at 3 in the
original code — but it did **not** reproduce as a sanitizer crash in this
environment across 60+ attempts (including deliberately widening the
teardown window with large boards and cooldowns): on Linux/glibc, `exit()`
following `return` from `main()` tears down the whole process and reaps all
threads — detached and still sleeping ones included — before they get a
chance to run further, so the dangling write essentially never gets to
execute in practice here. That's a property of this particular process-exit
implementation, not a guarantee of the language, so it was fixed rather
than left as a "doesn't crash on my machine" latent bug.

Fixed by removing the thread entirely: cooldown is now a per-agent
`board.next_shot_ok[t]` timestamp (`vector<time_point<steady_clock>>`),
guarded by the same `board.mtx` as `frozen_until`, and checked the same way
freeze already was. No thread spawned per shot, no shared state that can
outlive its owner, and cooldowns are correctly independent per agent.

### 3. `--max-steps` was a no-op

Parsed into `Options::maxSteps`, clamped, and then never read anywhere else
in `main()`. Not a sanitizer finding, but discovered while making tick
count genuinely CLI-configurable for stress runs (a stress request for
"100,000 ticks" was previously unachievable no matter what was passed).
Fixed: the main wait loop now also exits once the shared step counter
reaches `--max-steps`.

### 4. `--toons` capped at 3 / fixed-size identity tables

`TOON_CH`/`TOON_NM` were `vector<char>`/`vector<string>` of size 3, indexed
directly by toon index with no bounds check (`TOON_CH[t]`). Raising the
`--toons` cap without addressing this would turn "agent count is now
configurable" into a heap-buffer-overflow generator. Fixed by generating
per-agent glyph/name (`assignIdentities`) sized to `opt.toons` at startup.

## Suppressions

None. No warning was silenced — every finding above was fixed at the root
cause.
