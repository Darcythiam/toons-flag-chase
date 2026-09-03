# toons-flag-chase — README

Multithreaded cartoon race simulation with three Disney-inspired characters, each having unique abilities, rendered as **stacked ASCII boards** in the console.

---

## 🎮 Gameplay

* **RoadRunner (R)** — lightning fast and occasionally bursts with an extra step.
* **Coyote (C)** — can **jump over** a blocked cell (like walls or other characters) sometimes.
* **YosemiteSam (Y)** — can **shoot** and **freeze** another Toon for a short duration.

The race ends when any Toon reaches the flag (`F`) at the right edge of the board.

---

## ⚙️ Features

* **Thread-per-character concurrency** — each Toon runs on its own thread.
* **Stacked ASCII frame rendering** — each update prints a full board followed by a blank line.
* **Event feed** — logs when Coyote jumps or YosemiteSam shoots.
* **Dynamic tuning** via command-line arguments.

---

## 🧩 Command-Line Options

```
--rows N             Grid height (default 18)
--cols N             Grid width (default 36)
--toons N            Number of Toons (default 3: R, C, Y)
--delay-ms N         Delay between frames (default 120)
--max-steps N        Limit on total steps before stop (default 10000)
--shoot-chance X     YosemiteSam shooting chance (default 0.15)
--shoot-cooldown N   Cooldown in ms between shots (default 1500)
--freeze-ms N        Freeze duration after being hit (default 1000)
--jump-chance X      Coyote jump chance (default 0.25)
--seed N             Random seed (default system-generated)
```

---

## 🚀 Build Instructions

### Option 1: Using CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/toons
```

### Option 2: Using g++ directly

```bash
cd src
g++ -std=c++17 -O2 -pthread main.cpp -o ../toons
cd ..
./toons
```

---

## 🧠 Example Runs

```bash
# Default (stacked frames, moderate delay)
./toons

# Faster output
./toons --delay-ms 60

# Aggressive abilities
./toons --shoot-chance 0.25 --freeze-ms 1200 --jump-chance 0.35
```

---

## 🧱 Output Example

```
+------------------------------------+
|.R...........#..................F..|
|...................................|
|..#.................Y..............|
|...........C.......................|
+------------------------------------+
steps: 57

[Update] YosemiteSam shoots Coyote — frozen for 1000 ms

+------------------------------------+
|..R..............................F.|
|...................................|
|...................................|
|...........C.......................|
+------------------------------------+
steps: 63
```

---

## 🧮 Notes

* Each Toon runs independently with mutex synchronization.
* YosemiteSam’s cooldown and freeze state are per-agent timestamps checked
  under `board.mtx` — no timer threads are spawned (see Concurrency
  Invariants below for why that changed).
* Coyote’s jump only triggers if movement is blocked.
* RoadRunner’s burst step is small but frequent, making him visually faster.

---

## 🔒 Concurrency Invariants

This is a **single-global-lock design**: `board.mtx` is the only lock
guarding shared board state, and every access to it goes through the same
mutex — agent positions (`toonPos`), freeze deadlines (`frozen_until`),
shoot cooldowns (`next_shot_ok`), per-agent step counts (`steps`), and the
render grid (`cell`/`grid`, via `rebuild_grid`). There is no per-agent or
per-field sharding; a worker thread touching any of that state acquires the
exact same `board.mtx` that every other worker acquires.

A second mutex, `render_mtx`, exists purely to serialize `stdout` writes
inside `log_event` (the small "[Update] ... " event lines). It is
completely independent of board state — it protects nothing but `cout`.

**Lock order.** The two call sites of `log_event` (Coyote's jump message
and YosemiteSam's shoot message, in `src/main.cpp`) both execute while the
surrounding `board.mtx` critical section is still open, so `render_mtx` is
always acquired *underneath* an already-held `board.mtx`. The invariant is:

> `board.mtx` is always acquired before `render_mtx`; `render_mtx` is never
> held while attempting to acquire `board.mtx`.

Because there is exactly one lock in the board-state critical path
(`render_mtx` never nests the other way around it), lock-order deadlock is
structurally impossible here by construction, not by convention — there's
no second board-state lock to acquire out of order. To keep it that way as
the code changes, every acquisition site uses `BoardLock`/`RenderLock`
(thin RAII wrappers around `board.mtx`/`board.render_mtx` in
`src/main.cpp`) instead of a bare `lock_guard`. `RenderLock` sets a
thread-local flag while `render_mtx` is held; `BoardLock` asserts that flag
is clear before locking `board.mtx`. In a debug build (no `-DNDEBUG` — true
for the TSan/ASan configs in `docs/SANITIZERS.md`, false for the default
Release build) a future contributor who adds a `render_mtx`-then-`board.mtx`
path will hit that assertion immediately instead of introducing a latent
deadlock.

**Why this matters, concretely:** the single-lock design is *why* the
`frozen_until` data race documented in `docs/SANITIZERS.md` was possible in
the first place. It wasn't a case of two locks racing each other or being
acquired out of order — `board.mtx` already covered every write to
`frozen_until`. The bug was a *read* of `board.frozen_until[t]` that simply
never took `board.mtx` at all, bypassing the one lock that existed rather
than exposing a gap between two locks. The fix was closing that unguarded
read (taking `board.mtx` for it, same as every other access), not adding a
second lock — there was, and still is, only one lock guarding board state.

