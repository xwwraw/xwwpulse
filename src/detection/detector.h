//  The hybrid detection pipeline. Driven by events (foreground facts from
//  the agent, process exits from ETW and from a wait on the game); the
//  only timer is Tick() while a candidate waits for its frame rate.
//    Level 0  cached verdict (PID + creation time), Always / Never optimize lists
//    Level 1  non-game list, stop modules, score from window, Windows game list,
//             launcher libraries and static facts
//    Level 2  average ETW present rate >= min_fps over confirm_seconds
#pragma once

#include "config/config.h"
#include "detection/etw_tracker.h"
#include "detection/facts.h"
#include "detection/heuristics.h"
#include "detection/library.h"
#include "detection/lru_cache.h"

#include <map>

namespace xww {

struct GameInfo {
    ProcessId id;
    std::wstring exe, name;         // full path, friendly name from the version resource
    int score = 0;
    std::wstring reasons;
    std::wstring antiCheat;         // empty when none was found
    bool forced = false;            // from the Always optimize list
};

class Detector {
public:
    explicit Detector(EtwTracker& etw) : etw_(etw) { instance_ = this; }
    ~Detector();

    std::function<void(const GameInfo&)> OnGameStarted;
    std::function<void(const GameInfo&, bool front)> OnGameFocus;
    std::function<void(const GameInfo&)> OnGameEnded;
    std::function<void(const std::wstring& exe, const std::wstring& verdict, const std::wstring& reasons)> OnVerdict;

    void SetConfig(const Config& config);
    void SetLists(std::vector<std::wstring> forced, std::vector<std::wstring> blocked);   // exe paths or file names
    void Foreground(const WindowFacts& facts);
    void ProcessExited(DWORD pid);
    void Tick();
    bool Confirming() const;
    DWORD TickInterval() const;     // 250 ms while a candidate is new, then 1 s
    void Forget();                  // drop cached verdicts (lists changed)

    std::optional<GameInfo> Game() const;
    bool GameInFront() const { return gameFront_; }
    std::wstring Phase() const;     // "idle" | "confirming" | "game" | "paused"

private:
    enum class List { None, Forced, Blocked };
    struct Verdict { bool game; GameInfo info; };
    struct Candidate { GameInfo info; ULONGLONG since = 0; bool reported = false; };
    using Events = std::vector<std::function<void()>>;

    void Evaluate(const WindowFacts& f, Events& out);
    void Confirm(GameInfo info, Events& out);
    void Reject(const ProcessId& id, const std::wstring& exe, const std::wstring& reason, Events& out, bool cache = true);
    void EndGame(Events& out);
    List Classify(const std::wstring& exe) const;
    void UpdatePresent();
    void WatchExit(DWORD pid);
    void StopWatchingExit();
    static void CALLBACK OnExit(PVOID context, BOOLEAN);
    static void Run(Events& events) { for (auto& e : events) e(); }

    EtwTracker& etw_;
    GameLibrary library_;
    Config config_ = Config::Defaults();
    mutable std::recursive_mutex gate_;
    LruCache<ProcessId, Verdict, ProcessIdHash> cache_{ 256 };
    std::vector<std::wstring> forced_, blocked_;
    std::optional<Candidate> candidate_;
    std::optional<GameInfo> game_;
    bool gameFront_ = false;
    WindowFacts front_;
    std::map<DWORD, std::wstring> logged_;      // last reason logged per process, so re-probes do not repeat it
    Handle exitProcess_;
    HANDLE exitWait_ = nullptr;
    static inline std::atomic<Detector*> instance_{ nullptr };   // the service has one detector; exit waits find it here
};

}  // namespace xww
