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
* YosemiteSam’s cooldown and freeze logic run on separate detached threads.
* Coyote’s jump only triggers if movement is blocked.
* RoadRunner’s burst step is small but frequent, making him visually faster.

