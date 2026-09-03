#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <csignal>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <fstream>

using namespace std;
using namespace std::chrono;

struct Pos { int r; int c; };

struct Options {
    int rows = 18;
    int cols = 36;            // close to your sample width
    int toons = 3;            // R (RoadRunner), C (Coyote), Y (YosemiteSam)
    int maxSteps = 10000;
    unsigned int seed = std::random_device{}();

    // Output pacing & stacked style
    bool stacked = true;      // print NEW board for each update (matches your sample)
    bool render = true;       // set false (--no-render) to skip board/event I/O for stress runs
    int delay_ms = 120;       // wait between printed boards (enhances realism)

    // Ability tuning
    double rr_burst_chance = 0.15;     // RoadRunner burst (extra step)
    double coy_jump_chance = 0.25;     // Coyote jump when blocked
    double sam_shoot_chance = 0.15;    // YosemiteSam may shoot
    int    sam_cooldown_ms = 1500;     // cooldown between shots
    int    sam_freeze_ms   = 1000;     // freeze duration

    // Benchmark / sweep (see docs/SANITIZERS.md sibling: docs/BENCHMARKS.md)
    bool benchmark = false;            // --benchmark: N trials at --toons, report throughput+latency
    int  benchmark_trials = 10;        // --benchmark-trials N
    bool sweep = false;                // --sweep: run --benchmark across a list of agent counts
    string sweep_counts = "10,100,1000,10000,100000"; // --sweep-counts "a,b,c"
    string csv_out = "bench_global_mutex_baseline.csv"; // --csv-out PATH
};

static atomic<bool> gStop(false);
void on_sigint(int){ gStop.store(true); }

// Lock-order invariant (see README "Concurrency Invariants"): board.mtx is
// always acquired before render_mtx; render_mtx is never held while this
// thread attempts to acquire board.mtx. These RAII wrappers replace plain
// lock_guard<mutex> at every acquisition site so a future violation trips
// the assert below (debug builds only -- a no-op under NDEBUG) instead of
// silently opening a lock-order deadlock.
static thread_local bool holding_render_mtx = false;

#ifdef LOCK_INSTRUMENTATION
// Per-thread accumulation of time spent waiting to acquire board.mtx and
// time spent holding it, in nanoseconds. Only compiled in when the
// LOCK_INSTRUMENTATION CMake option is ON (a separate build from the one
// used for the real throughput/latency numbers -- see docs/BENCHMARKS.md
// for why: timing every lock acquisition/release adds overhead that would
// skew the very throughput figures the benchmark exists to measure).
// Each worker thread is 1:1 with one agent for its whole lifetime (no
// thread pooling), so these reset to zero naturally on every new trial.
static thread_local long long tlBoardWaitNs = 0;
static thread_local long long tlBoardHoldNs = 0;
#endif

struct BoardLock {
    explicit BoardLock(mutex &m) : m_(m) {
        assert(!holding_render_mtx &&
               "lock-order violation: board.mtx acquired while render_mtx held");
#ifdef LOCK_INSTRUMENTATION
        auto t0 = steady_clock::now();
        m_.lock();
        auto t1 = steady_clock::now();
        tlBoardWaitNs += duration_cast<nanoseconds>(t1 - t0).count();
        holdStart_ = t1;
#else
        m_.lock();
#endif
    }
    ~BoardLock(){
#ifdef LOCK_INSTRUMENTATION
        auto t1 = steady_clock::now();
        tlBoardHoldNs += duration_cast<nanoseconds>(t1 - holdStart_).count();
#endif
        m_.unlock();
    }
    BoardLock(const BoardLock&) = delete;
    BoardLock& operator=(const BoardLock&) = delete;
private:
    mutex &m_;
#ifdef LOCK_INSTRUMENTATION
    steady_clock::time_point holdStart_;
#endif
};

struct RenderLock {
    explicit RenderLock(mutex &m) : m_(m) {
        m_.lock();
        holding_render_mtx = true;
    }
    ~RenderLock(){ holding_render_mtx = false; m_.unlock(); }
    RenderLock(const RenderLock&) = delete;
    RenderLock& operator=(const RenderLock&) = delete;
private:
    mutex &m_;
};

struct Board {
    int R, C;
    vector<string> grid;                 // render buffer
    vector<vector<char>> cell;           // static cells ('.', '#', '|', 'F')
    int finishCol;                       // right wall
    Pos flag;                            // goal

    mutex mtx;                           // state lock
    mutex render_mtx;                    // serialize printing

    vector<Pos> toonPos;                 // per-toon position
    vector<time_point<steady_clock>> frozen_until;   // guarded by mtx
    vector<time_point<steady_clock>> next_shot_ok;   // guarded by mtx: per-toon shoot cooldown
    vector<int> steps;                   // per-toon step count
    vector<char> toonCh;                 // per-toon glyph
    vector<string> toonNm;               // per-toon display name

    Board(int r, int c, int nToons)
      : R(r), C(c), grid(r, string(c, '.')), cell(r, vector<char>(c, '.')),
        finishCol(c-1), toonPos(nToons), frozen_until(nToons),
        next_shot_ok(nToons), steps(nToons,0) {
        flag = {R/2, C-2};
        for (int t=0;t<nToons;t++){
            frozen_until[t] = steady_clock::time_point::min();
            next_shot_ok[t] = steady_clock::time_point::min();
        }
    }
    bool inBounds(int r, int c) const { return (r>=0 && r<R && c>=0 && c<C); }
};

// Ability role — cycles every 3 toons so large --toons counts still exercise
// all three ability code paths (shoot/freeze, jump, burst) under stress.
enum Toon { ROADRUNNER=0, COYOTE=1, YOSEMITESAM=2 };

static void assignIdentities(Board &b, int nToons){
    static const char*  names[3] = {"RoadRunner","Coyote","YosemiteSam"};
    static const char   chars[3] = {'R','C','Y'};
    // Extra glyphs for toons beyond the first 3 (cosmetic only; board rendering
    // isn't unique per-agent at high counts, which is fine since stress runs
    // use --no-render).
    static const char   extra[]  = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!$%&*+=?<>~^";
    constexpr int nExtra = sizeof(extra) - 1;
    b.toonCh.resize(nToons);
    b.toonNm.resize(nToons);
    for(int t=0; t<nToons; t++){
        int role = t % 3;
        if(t < 3){
            b.toonCh[t] = chars[role];
            b.toonNm[t] = names[role];
        } else {
            b.toonCh[t] = extra[(t-3) % nExtra];
            b.toonNm[t] = string(names[role]) + "#" + to_string(t);
        }
    }
}

static inline Pos pick_step(mt19937 &rng){
    static const Pos dirs[5] = {{-1,0},{1,0},{0,-1},{0,1},{0,0}}; // 4-neigh + stay
    uniform_int_distribution<int> dist(0,4);
    return dirs[dist(rng)];
}

Options parseArgs(int argc, char** argv){
    Options o;
    auto takeInt = [&](int &out, int i, char** argv){ out = stoi(argv[i]); };
    for(int i=1;i<argc;i++){
        string a = argv[i];
        auto next = [&](int &slot){ if(i+1<argc) takeInt(slot, ++i, argv); };
        if(a=="--rows") next(o.rows);
        else if(a=="--cols") next(o.cols);
        else if(a=="--toons") next(o.toons);
        else if(a=="--max-steps") next(o.maxSteps);
        else if(a=="--seed") { if(i+1<argc) o.seed = (unsigned)stoul(argv[++i]); }
        else if(a=="--delay-ms") next(o.delay_ms);
        else if(a=="--shoot-chance") { if(i+1<argc) o.sam_shoot_chance = stod(argv[++i]); }
        else if(a=="--shoot-cooldown") next(o.sam_cooldown_ms);
        else if(a=="--freeze-ms") next(o.sam_freeze_ms);
        else if(a=="--jump-chance") { if(i+1<argc) o.coy_jump_chance = stod(argv[++i]); }
        else if(a=="--no-render") o.render = false;
        else if(a=="--benchmark") o.benchmark = true;
        else if(a=="--benchmark-trials") next(o.benchmark_trials);
        else if(a=="--sweep") o.sweep = true;
        else if(a=="--sweep-counts") { if(i+1<argc) o.sweep_counts = argv[++i]; }
        else if(a=="--csv-out") { if(i+1<argc) o.csv_out = argv[++i]; }
        else if(a=="--help"){
            cout << "Options\n"
                 << "  --rows N             (default 18)\n"
                 << "  --cols N             (default 36)\n"
                 << "  --toons N            (default 3; any positive N, roles R/C/Y cycle)\n"
                 << "  --max-steps N        (default 10000; hard cap on total steps taken;\n"
                 << "                        also the per-trial tick budget in --benchmark/--sweep)\n"
                 << "  --seed N             (default time)\n"
                 << "  --delay-ms N         (default 120)\n"
                 << "  --shoot-chance X     (default 0.15)\n"
                 << "  --shoot-cooldown N   (ms, default 1500)\n"
                 << "  --freeze-ms N        (default 1000)\n"
                 << "  --jump-chance X      (default 0.25)\n"
                 << "  --no-render          (skip per-frame board/event printing; for stress runs)\n"
                 << "  --benchmark          (run --benchmark-trials trials at --toons, headless,\n"
                 << "                        forces delay-ms=0; reports throughput + frame latency)\n"
                 << "  --benchmark-trials N (default 10)\n"
                 << "  --sweep              (run --benchmark across --sweep-counts, write --csv-out)\n"
                 << "  --sweep-counts LIST  (comma-separated agent counts, default \"10,100,1000,10000,100000\")\n"
                 << "  --csv-out PATH       (default bench_global_mutex_baseline.csv)\n";
            exit(0);
        }
    }
    o.toons = max(1, o.toons);
    o.rows = max(5, o.rows);
    o.cols = max(20, o.cols);
    o.maxSteps = max(100, o.maxSteps);
    o.benchmark_trials = max(1, o.benchmark_trials);
    return o;
}

static void printConfig(const Options &o){
    cout << "=== Config ===\n"
         << "rows=" << o.rows << " cols=" << o.cols << " toons=" << o.toons
         << " max-steps=" << o.maxSteps << " seed=" << o.seed << "\n"
         << "delay-ms=" << o.delay_ms << " render=" << (o.render ? "on" : "off") << "\n"
         << "rr-burst-chance=" << o.rr_burst_chance
         << " jump-chance=" << o.coy_jump_chance
         << " shoot-chance=" << o.sam_shoot_chance
         << " shoot-cooldown-ms=" << o.sam_cooldown_ms
         << " freeze-ms=" << o.sam_freeze_ms << "\n";
    if(o.benchmark || o.sweep){
        cout << "benchmark=" << (o.benchmark?"on":"off") << " sweep=" << (o.sweep?"on":"off")
             << " benchmark-trials=" << o.benchmark_trials;
        if(o.sweep) cout << " sweep-counts=" << o.sweep_counts << " csv-out=" << o.csv_out;
        cout << "\n"
             << "note: benchmark/sweep trials force render=off delay-ms=0 internally\n"
                "      regardless of the render/delay-ms values printed above\n";
    }
    cout << "\n";
    cout.flush();
}

static void rebuild_grid(Board &b){
    for(int r=0;r<b.R;r++) for(int c=0;c<b.C;c++) b.grid[r][c] = b.cell[r][c];
    for(int r=0;r<b.R;r++) b.grid[r][b.finishCol] = '|';
    b.grid[b.flag.r][b.flag.c] = 'F';
    for(size_t t=0;t<b.toonPos.size();t++){
        auto p = b.toonPos[t]; b.grid[p.r][p.c] = b.toonCh[t];
    }
}

static void print_board(const Board &b, int totalSteps){
    cout << "+" << string(b.C, '-') << "+\n";
    for(int r=0;r<b.R;r++){
        cout << "|";
        for(int c=0;c<b.C;c++) cout << b.grid[r][c];
        cout << "|\n";
    }
    cout << "+" << string(b.C, '-') << "+\n";
    cout << "steps: " << totalSteps << "\n\n"; // extra blank line between boards
    cout.flush();
}

// Streaming (Welford) mean/variance, plus the parallel-combine formula
// (Chan et al.) so per-trial latency distributions can be pooled into one
// overall mean/stddev across an entire --benchmark or --sweep run without
// storing every individual sample.
struct Welford {
    long long n = 0;
    double mean = 0.0, M2 = 0.0;
    void add(double x){
        n++;
        double d = x - mean;
        mean += d / n;
        double d2 = x - mean;
        M2 += d * d2;
    }
    double stddev() const { return n > 1 ? sqrt(M2 / (n - 1)) : 0.0; }
};
static Welford combine(const Welford &a, const Welford &b){
    if(a.n == 0) return b;
    if(b.n == 0) return a;
    Welford c;
    c.n = a.n + b.n;
    double delta = b.mean - a.mean;
    c.mean = a.mean + delta * b.n / c.n;
    c.M2 = a.M2 + b.M2 + delta * delta * (double)a.n * b.n / c.n;
    return c;
}

struct TrialResult {
    long long ticks = 0;
    double elapsedSec = 0.0;
    double throughputTPS = 0.0;
    Welford latencyUs;          // inter-tick latency samples, microseconds
    double lockWaitPct = -1.0;  // -1 = not measured (LOCK_INSTRUMENTATION not compiled in)
    int winner = -1;
    vector<int> steps;
};

// Runs one full game/trial: builds a fresh Board, places walls and agents,
// spawns opt.toons worker threads, runs to completion, joins, and returns
// timing/outcome data. This is the single simulation core shared by the
// interactive single-run path, --benchmark, and --sweep -- it is NOT
// reimplemented separately for benchmarking, so the measured throughput
// and latency reflect the exact same lock-acquisition pattern (BoardLock/
// RenderLock, same critical sections) as normal play.
//
// opt.benchmark changes two things: the "reach the flag" win condition no
// longer ends the game early (the trial always runs the full opt.maxSteps
// ticks, giving every trial and every agent count a directly comparable,
// fixed amount of work), and inter-tick latency is sampled via the Welford
// accumulator below. Everything else -- movement, jump, shoot/freeze,
// burst, locking -- is identical to non-benchmark play.
static TrialResult runOneGame(Options opt){
    Board board(opt.rows, opt.cols, opt.toons);
    assignIdentities(board, opt.toons);

    atomic<bool> gameOver(false);
    atomic<int> winner(-1);
    atomic<int> totalSteps(0);

    mt19937 rng(opt.seed);
    uniform_int_distribution<int> rr(0, board.R-1), cc(0, board.C-3);

    // Sprinkle a few walls so jumps matter (about 3%)
    int numWalls = (board.R*board.C)/30;
    for(int i=0;i<numWalls;i++){
        int r = rr(rng), c = cc(rng);
        if(r==board.flag.r && c==board.flag.c){ --i; continue; }
        board.cell[r][c] = '#';
    }

    // Random starting positions. Bounded-attempt rejection sampling with a
    // linear-scan fallback so a dense board (e.g. --toons close to rows*cols)
    // can't spin the do-while loop forever.
    {
        BoardLock lk(board.mtx);
        vector<vector<bool>> used(board.R, vector<bool>(board.C, false));
        used[board.flag.r][board.flag.c] = true;
        for(int t=0;t<opt.toons;t++){
            int r=-1, c=-1; bool found=false;
            for(int attempt=0; attempt<10000 && !found; attempt++){
                int rr_ = rr(rng), cc_ = cc(rng);
                if(!used[rr_][cc_] && board.cell[rr_][cc_] != '#'){ r=rr_; c=cc_; found=true; }
            }
            if(!found){
                for(int r2=0; r2<board.R && !found; r2++)
                    for(int c2=0; c2<=board.C-3 && !found; c2++)
                        if(!used[r2][c2] && board.cell[r2][c2] != '#'){ r=r2; c=c2; found=true; }
            }
            if(!found){
                cerr << "Error: board too small to place " << opt.toons
                     << " toons; increase --rows/--cols.\n";
                exit(1);
            }
            used[r][c]=true; board.toonPos[t] = {r,c};
        }
    }

    rebuild_grid(board);
    if(opt.render) print_board(board, totalSteps.load());

    auto now = []{ return steady_clock::now(); };

    auto log_event = [&](const string &msg){
        RenderLock lk(board.render_mtx);
        cout << msg << "\n\n"; // small text + space after
        cout.flush();
        this_thread::sleep_for(milliseconds(opt.delay_ms)); // respect pacing when logging
    };

    // Inter-tick latency accumulator (benchmark mode only). Every totalSteps
    // increment already happens while board.mtx is held (all three sites
    // below are inside a BoardLock scope), so recording "time since the
    // previous tick" here needs no extra synchronization -- it rides the
    // same critical section that was already exclusive.
    //
    // `measuring` brackets sample collection to exactly the same
    // [trialStart, trialEnd] window used for the tick-count/throughput
    // measurement below (both flip via this one flag). Without this, a
    // tick from thread spawn (before trialStart) or from the post-signal
    // drain period (worker threads only re-check gameOver at the top of
    // their loop, so up to ~toons of them can each complete one more tick
    // after trialEnd before exiting) would be sampled too -- this was
    // caught empirically: latency sample counts were running ~2x the
    // configured ticks/trial before this flag was added.
    atomic<bool> measuring(false);
    Welford latencyUs;
    bool haveLastTick = false;
    time_point<steady_clock> lastTick;
    auto recordTick = [&](){
        if(!opt.benchmark || !measuring.load()) return;
        auto t = now();
        if(haveLastTick) latencyUs.add(duration<double, micro>(t - lastTick).count());
        haveLastTick = true;
        lastTick = t;
    };

#ifdef LOCK_INSTRUMENTATION
    // Per-thread wait time and the thread's OWN lifetime (creation to exit),
    // not the trial's [trialStart, trialEnd] window: tlBoardWaitNs starts
    // accumulating the moment a thread is created (before the main thread
    // even finishes spawning the rest), so dividing it by the trial's
    // elapsed time produced nonsensical >100% wait percentages (found
    // empirically: 110% and 196% at toons=1000/10000). Measuring each
    // thread against its own start-to-exit span keeps numerator and
    // denominator in the same window, so the ratio is always in [0,100].
    vector<long long> trialWaitNs(opt.toons, 0), trialHoldNs(opt.toons, 0);
    vector<double> trialThreadLifeSec(opt.toons, 0.0);
#endif

    auto worker = [&](int t){
#ifdef LOCK_INSTRUMENTATION
        auto threadStart = steady_clock::now();
#endif
        int role = t % 3;
        mt19937 trng(opt.seed + 777u*(t+1));
        uniform_real_distribution<double> chance(0.0, 1.0);

        // Visual pacing per toon (RoadRunner-role is fastest)
        milliseconds base_sleep(70);
        if(role==ROADRUNNER) base_sleep = milliseconds(35);
        else if(role==COYOTE) base_sleep = milliseconds(60);
        else if(role==YOSEMITESAM) base_sleep = milliseconds(75);

        while(!gameOver.load() && !gStop.load()){
            // If frozen, just wait. Read frozen_until under the same mutex
            // that protects its writes (fixes a TSan-detected data race:
            // this used to be an unlocked read racing the locked write
            // below when YosemiteSam freezes a target).
            bool isFrozen;
            {
                BoardLock lk(board.mtx);
                isFrozen = now() < board.frozen_until[t];
            }
            if(isFrozen){ this_thread::sleep_for(base_sleep); continue; }

            // Bias toward flag most of the time
            Pos step{0,0};
            {
                BoardLock lk(board.mtx);
                Pos cur = board.toonPos[t];
                Pos dir{ (board.flag.r > cur.r) - (board.flag.r < cur.r),
                         (board.flag.c > cur.c) - (board.flag.c < cur.c) };
                if (chance(trng) < 0.70) {
                    if (uniform_int_distribution<int>(0,1)(trng)==0 && dir.r!=0) step={dir.r,0};
                    else if(dir.c!=0) step={0,dir.c};
                    else step=pick_step(trng);
                } else step=pick_step(trng);
            }

            bool moved=false;
            {
                BoardLock lk(board.mtx);
                Pos cur = board.toonPos[t];
                Pos nxt{cur.r + step.r, cur.c + step.c};
                auto occ = [&](int r,int c){ for(size_t k=0;k<board.toonPos.size();k++) if((int)k!=t){ if(board.toonPos[k].r==r && board.toonPos[k].c==c) return true;} return false; };

                auto try_move = [&](Pos dest){
                    if(board.inBounds(dest.r,dest.c) && dest.c < board.finishCol && board.cell[dest.r][dest.c] != '#' && !occ(dest.r,dest.c)){
                        board.toonPos[t] = dest; board.steps[t]++; moved=true; return true; }
                    return false; };

                bool blocked = !(board.inBounds(nxt.r,nxt.c) && nxt.c < board.finishCol) || board.cell[nxt.r][nxt.c]=='#' || occ(nxt.r,nxt.c);

                // Coyote-role: jump over one cell sometimes when blocked
                if(blocked && role==COYOTE && chance(trng) < opt.coy_jump_chance){
                    Pos hop{nxt.r + step.r, nxt.c + step.c};
                    if(try_move(hop)){
                        rebuild_grid(board);
                        int ts = ++totalSteps;
                        recordTick();
                        if(opt.render){
                            print_board(board, ts);
                            // board.mtx (BoardLock above) is still held here; log_event
                            // acquires render_mtx underneath it. board.mtx -> render_mtx
                            // is the only permitted order (see README).
                            log_event("[Update] " + board.toonNm[t] + " jumps to (" + to_string(hop.r) + "," + to_string(hop.c) + ")");
                        }
                    }
                }
                // Normal move
                if(!moved && try_move(nxt)){
                    rebuild_grid(board); int ts = ++totalSteps;
                    recordTick();
                    if(opt.render) print_board(board, ts);
                }

                // Win check (skipped in benchmark mode: every trial runs the
                // full opt.maxSteps ticks regardless of who's closest to the
                // flag, so all trials and all agent counts do the same
                // amount of work and are directly comparable).
                if(!opt.benchmark && !gameOver.load()){
                    Pos p = board.toonPos[t];
                    if((p.r==board.flag.r && p.c==board.flag.c) || p.c >= board.finishCol-1){
                        winner.store(t); gameOver.store(true);
                    }
                }
            }

            // YosemiteSam-role: fire & freeze with a per-toon cooldown.
            // Cooldown is tracked as a timestamp guarded by board.mtx instead
            // of the previous atomic<bool> + detached timer thread: that
            // detached thread captured stack variables from main() by
            // reference and could still be sleeping when main() returned,
            // writing to already-destroyed stack memory (ASan
            // stack-use-after-scope). A timestamp compared under the same
            // lock as frozen_until needs no extra thread and can't dangle.
            if(role==YOSEMITESAM && !gameOver.load()){
                bool onCooldown;
                {
                    BoardLock lk(board.mtx);
                    onCooldown = now() < board.next_shot_ok[t];
                }
                if(!onCooldown && chance(trng) < opt.sam_shoot_chance){
                    int target=-1; int bestD=1e9;
                    {
                        BoardLock lk(board.mtx);
                        for(int k=0;k<opt.toons;k++) if(k!=t){
                            if(now() < board.frozen_until[k]) continue;
                            int d = abs(board.toonPos[k].r - board.toonPos[t].r) + abs(board.toonPos[k].c - board.toonPos[t].c);
                            if(d < bestD){ bestD=d; target=k; }
                        }
                        board.next_shot_ok[t] = now() + milliseconds(opt.sam_cooldown_ms);
                        if(target!=-1){
                            board.frozen_until[target] = now() + milliseconds(opt.sam_freeze_ms);
                            rebuild_grid(board); // show positions when shot happens
                            int ts = totalSteps.load();
                            if(opt.render){
                                print_board(board, ts);
                                // board.mtx (BoardLock above) is still held here; same
                                // board.mtx -> render_mtx order as the Coyote jump case.
                                log_event("[Update] " + board.toonNm[t] + " shoots " + board.toonNm[target] + " — frozen for " + to_string(opt.sam_freeze_ms) + " ms");
                            }
                        }
                    }
                }
            }

            // RoadRunner-role: occasional burst (extra step toward flag)
            if(role==ROADRUNNER && moved && !gameOver.load()){
                if(uniform_real_distribution<double>(0.0,1.0)(trng) < opt.rr_burst_chance){
                    BoardLock lk(board.mtx);
                    Pos cur = board.toonPos[t];
                    Pos dir{ (board.flag.r > cur.r) - (board.flag.r < cur.r),
                             (board.flag.c > cur.c) - (board.flag.c < cur.c) };
                    Pos step2 = (abs(dir.r)+abs(dir.c) ? Pos{ (dir.r!=0)?dir.r:0, (dir.r==0)?dir.c:0 } : pick_step(trng));
                    Pos nxt{cur.r + step2.r, cur.c + step2.c};

                    auto occ = [&](int r,int c){ for(size_t k=0;k<board.toonPos.size();k++) if((int)k!=t){ if(board.toonPos[k].r==r && board.toonPos[k].c==c) return true;} return false; };
                    if(board.inBounds(nxt.r,nxt.c) && nxt.c < board.finishCol && board.cell[nxt.r][nxt.c] != '#' && !occ(nxt.r,nxt.c)){
                        board.toonPos[t] = nxt; board.steps[t]++;
                        rebuild_grid(board); int ts = ++totalSteps;
                        recordTick();
                        if(opt.render) print_board(board, ts);
                    }
                }
            }

            // Global pacing so stacked frames feel smooth
            if(opt.delay_ms > 0) this_thread::sleep_for(milliseconds(opt.delay_ms));
        }

#ifdef LOCK_INSTRUMENTATION
        if(opt.benchmark){
            trialWaitNs[t] = tlBoardWaitNs;
            trialHoldNs[t] = tlBoardHoldNs;
            trialThreadLifeSec[t] = duration<double>(steady_clock::now() - threadStart).count();
        }
#endif
    };

    vector<thread> workers; workers.reserve(opt.toons);
    time_point<steady_clock> trialStart{}, trialEnd{};
    long long ticksAtStart = 0, ticksAtEnd = 0;
    for(int t=0;t<opt.toons;t++) workers.emplace_back(worker, t);
    if(opt.benchmark){
        // Snapshot both the tick counter and the clock together, excluding
        // thread-creation overhead from the measured window.
        ticksAtStart = totalSteps.load();
        trialStart = now();
        measuring.store(true);
    }

    if(opt.benchmark){
        // Gate on the WINDOWED tick count (since ticksAtStart), not the raw
        // counter -- otherwise ticks produced during thread spawn (before
        // trialStart) would eat into the budget and shorten the measured
        // window at high agent counts, making throughput noisier exactly
        // where it matters most.
        while((totalSteps.load() - ticksAtStart) < opt.maxSteps && !gStop.load()){
            this_thread::sleep_for(microseconds(200));
        }
        trialEnd = now();
        ticksAtEnd = totalSteps.load();
        measuring.store(false);
        gameOver.store(true);
    } else {
        // Stop on: a winner, Ctrl+C, or hitting the step cap (max-steps was
        // previously parsed but never enforced anywhere — a no-op flag).
        while(!gameOver.load() && !gStop.load() && totalSteps.load() < opt.maxSteps){
            this_thread::sleep_for(milliseconds(5));
        }
        gameOver.store(true); // release any workers still waiting on gameOver (step-limit / Ctrl+C cases)
    }
    for(auto &th : workers) th.join(); // join excluded from timed window too

    TrialResult res;
    res.winner = winner.load();
    res.steps.assign(board.steps.begin(), board.steps.end());
    if(opt.benchmark){
        // Ticks counted here are exactly those produced within
        // [trialStart, trialEnd] -- NOT totalSteps.load() taken after join,
        // which would include however many extra ticks in-flight worker
        // threads completed while draining down to their next gameOver
        // check (up to ~toons extra ticks at high agent counts: found this
        // via a ~2x overshoot at toons=10000 before this fix landed).
        res.ticks = ticksAtEnd - ticksAtStart;
        res.elapsedSec = duration<double>(trialEnd - trialStart).count();
        res.throughputTPS = res.elapsedSec > 0 ? res.ticks / res.elapsedSec : 0.0;
        res.latencyUs = latencyUs;
#ifdef LOCK_INSTRUMENTATION
        {
            double sumPct = 0; int counted = 0;
            for(int t=0;t<opt.toons;t++){
                if(trialThreadLifeSec[t] > 0){
                    sumPct += (trialWaitNs[t] / 1e9) / trialThreadLifeSec[t] * 100.0;
                    counted++;
                }
            }
            res.lockWaitPct = counted > 0 ? sumPct / counted : 0.0;
        }
#endif
    } else {
        res.ticks = totalSteps.load();
    }

    if(!opt.benchmark){
        bool stepLimitReached = res.winner < 0 && !gStop.load() && res.ticks >= opt.maxSteps;
        if(opt.render){
            rebuild_grid(board);
            print_board(board, res.ticks);
        }
        cout << "=== Final Summary ===\n";
        cout << "total steps: " << res.ticks << "\n";
        if(res.winner>=0){
            for(int t=0;t<opt.toons;t++) cout << board.toonNm[t] << " (" << board.toonCh[t] << ") steps: " << board.steps[t] << "\n";
            cout << "Winner: " << board.toonNm[res.winner] << "\n";
        } else if(stepLimitReached){
            cout << "No winner — step limit (" << opt.maxSteps << ") reached.\n";
        } else {
            cout << "No winner — interrupted.\n";
        }
    }
    return res;
}

struct AggResult {
    int toons = 0;
    int trials = 0;
    long long ticksPerTrial = 0;
    double throughputMean = 0.0, throughputStddev = 0.0;
    double latencyMeanUs = 0.0, latencyStddevUs = 0.0;
    long long latencyN = 0;
    double lockWaitPctMean = -1.0; // -1 = not measured
};

// Runs nTrials independent trials at opt.toons (each with a distinct seed:
// opt.seed + trial index, both to avoid replaying the exact same initial
// layout and because thread-scheduling variance means repeats of the same
// seed aren't bit-identical anyway -- see docs/BENCHMARKS.md), aggregates
// throughput as mean/stddev across trials, and pools every trial's inter-
// tick latency samples into one overall mean/stddev via Welford's parallel
// combine formula.
static AggResult runTrials(Options opt, int nTrials){
    opt.benchmark = true;
    opt.render = false;
    opt.delay_ms = 0;

    vector<double> throughputs;
    throughputs.reserve(nTrials);
    Welford pooledLatency;
    double lockWaitSum = 0.0;
    int lockWaitCount = 0;

    for(int i=0;i<nTrials;i++){
        Options trialOpt = opt;
        trialOpt.seed = opt.seed + (unsigned)i;
        TrialResult r = runOneGame(trialOpt);
        throughputs.push_back(r.throughputTPS);
        pooledLatency = combine(pooledLatency, r.latencyUs);
        if(r.lockWaitPct >= 0){ lockWaitSum += r.lockWaitPct; lockWaitCount++; }
    }

    AggResult agg;
    agg.toons = opt.toons;
    agg.trials = nTrials;
    agg.ticksPerTrial = opt.maxSteps;
    double sum = 0; for(double v : throughputs) sum += v;
    agg.throughputMean = sum / nTrials;
    double sq = 0; for(double v : throughputs) sq += (v - agg.throughputMean) * (v - agg.throughputMean);
    agg.throughputStddev = nTrials > 1 ? sqrt(sq / (nTrials - 1)) : 0.0;
    agg.latencyMeanUs = pooledLatency.mean;
    agg.latencyStddevUs = pooledLatency.stddev();
    agg.latencyN = pooledLatency.n;
    agg.lockWaitPctMean = lockWaitCount > 0 ? lockWaitSum / lockWaitCount : -1.0;
    return agg;
}

static void printAgg(const AggResult &a){
    cout << "=== Benchmark: toons=" << a.toons << " trials=" << a.trials
         << " ticks/trial=" << a.ticksPerTrial << " ===\n";
    cout << fixed << setprecision(1);
    cout << "Throughput: mean=" << a.throughputMean << " ticks/sec  stddev="
         << a.throughputStddev << " ticks/sec\n";
    cout << setprecision(2);
    cout << "Frame latency: mean=" << a.latencyMeanUs << " us  stddev="
         << a.latencyStddevUs << " us  (n=" << a.latencyN << " samples)\n";
    if(a.lockWaitPctMean >= 0){
        cout << setprecision(1);
        cout << "Lock wait: mean=" << a.lockWaitPctMean
             << "% of thread wall-clock time blocked on board.mtx\n";
    }
    cout.unsetf(ios::fixed);
    cout << "\n";
    cout.flush();
}

static void runBenchmarkOnly(const Options &opt){
    AggResult a = runTrials(opt, opt.benchmark_trials);
    printAgg(a);
}

static vector<int> parseIntList(const string &s){
    vector<int> out;
    string cur;
    for(char c : s){
        if(c==','){ if(!cur.empty()){ out.push_back(stoi(cur)); cur.clear(); } }
        else cur += c;
    }
    if(!cur.empty()) out.push_back(stoi(cur));
    return out;
}

static void runSweep(const Options &opt){
    vector<int> counts = parseIntList(opt.sweep_counts);
    ofstream csv(opt.csv_out);
    csv << "agent_count,trials,ticks_per_trial,throughput_mean_tps,throughput_stddev_tps,"
           "latency_mean_us,latency_stddev_us,lock_wait_pct\n";
    for(int n : counts){
        Options o = opt;
        o.toons = n;
        AggResult a = runTrials(o, opt.benchmark_trials);
        printAgg(a);
        csv << a.toons << "," << a.trials << "," << a.ticksPerTrial << ","
            << a.throughputMean << "," << a.throughputStddev << ","
            << a.latencyMeanUs << "," << a.latencyStddevUs << ","
            << (a.lockWaitPctMean >= 0 ? to_string(a.lockWaitPctMean) : "") << "\n";
        csv.flush();
    }
    cout << "Sweep results written to " << opt.csv_out << "\n";
}

int main(int argc, char** argv){
    signal(SIGINT, on_sigint);
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    Options opt = parseArgs(argc, argv);
    printConfig(opt); // always printed, even with --no-render, so a stress run's
                       // actual effective parameters can be confirmed from stdout

    if(opt.sweep){ runSweep(opt); return 0; }
    if(opt.benchmark){ runBenchmarkOnly(opt); return 0; }

    runOneGame(opt);
    return 0;
}
