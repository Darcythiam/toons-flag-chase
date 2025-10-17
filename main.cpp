#include <algorithm>
#include <atomic>
#include <chrono>
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
    int delay_ms = 120;       // wait between printed boards (enhances realism)

    // Ability tuning
    double rr_burst_chance = 0.15;     // RoadRunner burst (extra step)
    double coy_jump_chance = 0.25;     // Coyote jump when blocked
    double sam_shoot_chance = 0.15;    // YosemiteSam may shoot
    int    sam_cooldown_ms = 1500;     // cooldown between shots
    int    sam_freeze_ms   = 1000;     // freeze duration
};

static atomic<bool> gStop(false);
void on_sigint(int){ gStop.store(true); }

struct Board {
    int R, C;
    vector<string> grid;                 // render buffer
    vector<vector<char>> cell;           // static cells ('.', '#', '|', 'F')
    int finishCol;                       // right wall
    Pos flag;                            // goal

    mutex mtx;                           // state lock
    mutex render_mtx;                    // serialize printing

    vector<Pos> toonPos;                 // R, C, Y positions
    vector<time_point<steady_clock>> frozen_until;
    vector<int> steps;                   // per-toon step count

    Board(int r, int c, int nToons)
      : R(r), C(c), grid(r, string(c, '.')), cell(r, vector<char>(c, '.')),
        finishCol(c-1), toonPos(nToons), frozen_until(nToons), steps(nToons,0) {
        flag = {R/2, C-2};
        for (int t=0;t<nToons;t++) frozen_until[t] = steady_clock::time_point::min();
    }
    bool inBounds(int r, int c) const { return (r>=0 && r<R && c>=0 && c<C); }
};

enum Toon { ROADRUNNER=0, COYOTE=1, YOSEMITESAM=2 }; // R, C, Y
static const vector<char>   TOON_CH = {'R','C','Y'};
static const vector<string> TOON_NM = {"RoadRunner","Coyote","YosemiteSam"};

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
        else if(a=="--help"){
            cout << "Options\n"
                 << "  --rows N             (default 18)\n"
                 << "  --cols N             (default 36)\n"
                 << "  --toons N            (default 3: R,C,Y)\n"
                 << "  --max-steps N        (default 10000)\n"
                 << "  --seed N             (default time)\n"
                 << "  --delay-ms N         (default 120)\n"
                 << "  --shoot-chance X     (default 0.15)\n"
                 << "  --shoot-cooldown N   (ms, default 1500)\n"
                 << "  --freeze-ms N        (default 1000)\n"
                 << "  --jump-chance X      (default 0.25)\n";
            exit(0);
        }
    }
    o.toons = max(1, min(o.toons, 3));
    o.rows = max(5, o.rows);
    o.cols = max(20, o.cols);
    o.maxSteps = max(100, o.maxSteps);
    return o;
}

static void rebuild_grid(Board &b){
    for(int r=0;r<b.R;r++) for(int c=0;c<b.C;c++) b.grid[r][c] = b.cell[r][c];
    for(int r=0;r<b.R;r++) b.grid[r][b.finishCol] = '|';
    b.grid[b.flag.r][b.flag.c] = 'F';
    for(size_t t=0;t<b.toonPos.size();t++){
        auto p = b.toonPos[t]; b.grid[p.r][p.c] = TOON_CH[t];
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

int main(int argc, char** argv){
    signal(SIGINT, on_sigint);
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    Options opt = parseArgs(argc, argv);
    Board board(opt.rows, opt.cols, opt.toons);

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

    // Random starting positions
    {
        lock_guard<mutex> lk(board.mtx);
        vector<vector<bool>> used(board.R, vector<bool>(board.C, false));
        used[board.flag.r][board.flag.c] = true;
        for(int t=0;t<opt.toons;t++){
            int r,c; do{ r=rr(rng); c=cc(rng);} while(used[r][c] || board.cell[r][c]=='#');
            used[r][c]=true; board.toonPos[t] = {r,c};
        }
    }

    rebuild_grid(board);
    print_board(board, totalSteps.load());

    // Ability state
    atomic<bool> sam_on_cd(false);
    auto now = []{ return steady_clock::now(); };

    auto log_event = [&](const string &msg){
        lock_guard<mutex> lk(board.render_mtx);
        cout << msg << "\n\n"; // small text + space after
        cout.flush();
        this_thread::sleep_for(milliseconds(opt.delay_ms)); // respect pacing when logging
    };

    auto worker = [&](int t){
        mt19937 trng(opt.seed + 777u*(t+1));
        uniform_real_distribution<double> chance(0.0, 1.0);

        // Visual pacing per toon (RoadRunner is fastest)
        milliseconds base_sleep(70);
        if(t==ROADRUNNER) base_sleep = milliseconds(35);
        else if(t==COYOTE) base_sleep = milliseconds(60);
        else if(t==YOSEMITESAM) base_sleep = milliseconds(75);

        while(!gameOver.load() && !gStop.load()){
            // If frozen, just wait
            if(now() < board.frozen_until[t]){ this_thread::sleep_for(base_sleep); continue; }

            // Bias toward flag most of the time
            Pos step{0,0};
            {
                lock_guard<mutex> lk(board.mtx);
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
            bool printed=false;
            {
                lock_guard<mutex> lk(board.mtx);
                Pos cur = board.toonPos[t];
                Pos nxt{cur.r + step.r, cur.c + step.c};
                auto occ = [&](int r,int c){ for(size_t k=0;k<board.toonPos.size();k++) if((int)k!=t){ if(board.toonPos[k].r==r && board.toonPos[k].c==c) return true;} return false; };

                auto try_move = [&](Pos dest){
                    if(board.inBounds(dest.r,dest.c) && dest.c < board.finishCol && board.cell[dest.r][dest.c] != '#' && !occ(dest.r,dest.c)){
                        board.toonPos[t] = dest; board.steps[t]++; moved=true; return true; }
                    return false; };

                bool blocked = !(board.inBounds(nxt.r,nxt.c) && nxt.c < board.finishCol) || board.cell[nxt.r][nxt.c]=='#' || occ(nxt.r,nxt.c);

                // Coyote: jump over one cell sometimes when blocked
                if(blocked && t==COYOTE && chance(trng) < opt.coy_jump_chance){
                    Pos hop{nxt.r + step.r, nxt.c + step.c};
                    if(try_move(hop)){
                        rebuild_grid(board);
                        int ts = ++totalSteps; printed=true;
                        print_board(board, ts);
                        log_event("[Update] Coyote jumps to (" + to_string(hop.r) + "," + to_string(hop.c) + ")");
                    }
                }
                // Normal move
                if(!moved && try_move(nxt)){
                    rebuild_grid(board); int ts = ++totalSteps; printed=true; print_board(board, ts);
                }

                // Win check
                if(!gameOver.load()){
                    Pos p = board.toonPos[t];
                    if((p.r==board.flag.r && p.c==board.flag.c) || p.c >= board.finishCol-1){
                        winner.store(t); gameOver.store(true);
                    }
                }
            }

            // YosemiteSam: fire & freeze with cooldown
            if(t==YOSEMITESAM && !gameOver.load()){
                if(!sam_on_cd.load() && chance(trng) < opt.sam_shoot_chance){
                    int target=-1; int bestD=1e9;
                    {
                        lock_guard<mutex> lk(board.mtx);
                        for(int k=0;k<opt.toons;k++) if(k!=t){
                            if(now() < board.frozen_until[k]) continue;
                            int d = abs(board.toonPos[k].r - board.toonPos[t].r) + abs(board.toonPos[k].c - board.toonPos[t].c);
                            if(d < bestD){ bestD=d; target=k; }
                        }
                        if(target!=-1){
                            board.frozen_until[target] = now() + milliseconds(opt.sam_freeze_ms);
                            rebuild_grid(board); // show positions when shot happens
                            int ts = totalSteps.load();
                            print_board(board, ts);
                            log_event("[Update] YosemiteSam shoots " + TOON_NM[target] + " — frozen for " + to_string(opt.sam_freeze_ms) + " ms");
                        }
                    }
                    sam_on_cd.store(true); std::thread([&]{ std::this_thread::sleep_for(std::chrono::milliseconds(opt.sam_cooldown_ms)); sam_on_cd.store(false); }).detach();
                }
            }

            // RoadRunner: occasional burst (extra step toward flag)
            if(t==ROADRUNNER && moved && !gameOver.load()){
                if(uniform_real_distribution<double>(0.0,1.0)(trng) < opt.rr_burst_chance){
                    lock_guard<mutex> lk(board.mtx);
                    Pos cur = board.toonPos[t];
                    Pos dir{ (board.flag.r > cur.r) - (board.flag.r < cur.r),
                             (board.flag.c > cur.c) - (board.flag.c < cur.c) };
                    Pos step2 = (abs(dir.r)+abs(dir.c) ? Pos{ (dir.r!=0)?dir.r:0, (dir.r==0)?dir.c:0 } : pick_step(trng));
                    Pos nxt{cur.r + step2.r, cur.c + step2.c};

                    auto occ = [&](int r,int c){ for(size_t k=0;k<board.toonPos.size();k++) if((int)k!=t){ if(board.toonPos[k].r==r && board.toonPos[k].c==c) return true;} return false; };
                    if(board.inBounds(nxt.r,nxt.c) && nxt.c < board.finishCol && board.cell[nxt.r][nxt.c] != '#' && !occ(nxt.r,nxt.c)){
                        board.toonPos[t] = nxt; board.steps[t]++;
                        rebuild_grid(board); int ts = ++totalSteps; print_board(board, ts);
                    }
                }
            }

            // Global pacing so stacked frames feel smooth
            this_thread::sleep_for(milliseconds(opt.delay_ms));
        }
    };

    vector<thread> workers; workers.reserve(opt.toons);
    for(int t=0;t<opt.toons;t++) workers.emplace_back(worker, t);

    while(!gameOver.load() && !gStop.load()) this_thread::sleep_for(milliseconds(5));
    for(auto &th : workers) th.join();

    // Final board
    rebuild_grid(board);
    print_board(board, totalSteps.load());
    if(winner.load()>=0){
        cout << "=== Final Summary ===\n";
        for(int t=0;t<opt.toons;t++) cout << TOON_NM[t] << " (" << TOON_CH[t] << ") steps: " << board.steps[t] << "\n";
        cout << "Winner: " << TOON_NM[winner.load()] << "\n";
    }
    return 0;
}
