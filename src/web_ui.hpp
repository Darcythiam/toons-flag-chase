#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct UiWall {
    int r = 0;
    int c = 0;
};

struct UiLayout {
    int rows = 0;
    int cols = 0;
    int flagR = 0;
    int flagC = 0;
    std::vector<UiWall> walls;
};

struct UiAgentView {
    int id = -1;
    int role = 0;
    std::string name;
    int r = 0;
    int c = 0;
    int steps = 0;
    bool frozen = false;
    bool canShoot = false;
    long long freezeRemainingMs = 0;
    long long cooldownRemainingMs = 0;
};

struct UiState {
    int rows = 0;
    int cols = 0;
    int totalAgents = 0;
    int displayedAgents = 0;
    int totalSteps = 0;
    int frozenAgents = 0;
    int delayMs = 0;
    int winner = -1;
    bool paused = false;
    bool gameOver = false;
    double elapsedSec = 0.0;
    std::vector<UiAgentView> agents;
    bool hasSelected = false;
    UiAgentView selected;
};

enum class UiCommandType {
    Pause,
    Resume,
    Stop,
    SetDelayMs,
    FreezeAgent,
    UnfreezeAgent,
    AddWall,
    RemoveWall
};

struct UiCommand {
    UiCommandType type = UiCommandType::Pause;
    int agentId = -1;
    int r = -1;
    int c = -1;
    int value = 0;
};

class WebUiServer {
public:
    using LayoutProvider = std::function<UiLayout()>;
    using StateProvider = std::function<UiState(int selectedAgent)>;
    using CommandSink = std::function<void(const UiCommand&)>;

    WebUiServer(int port,
                LayoutProvider layoutProvider,
                StateProvider stateProvider,
                CommandSink commandSink);
    ~WebUiServer();

    WebUiServer(const WebUiServer&) = delete;
    WebUiServer& operator=(const WebUiServer&) = delete;

    bool start();
    void stop();
    int port() const { return port_; }

private:
    void run();

    int port_;
    LayoutProvider layoutProvider_;
    StateProvider stateProvider_;
    CommandSink commandSink_;
    std::atomic<bool> stopping_{false};
    std::thread serverThread_;
    int listenFd_ = -1;
};
