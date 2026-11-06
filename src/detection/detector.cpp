#include "detection/detector.h"

namespace xww {

namespace {

constexpr ULONGLONG kSlowTickAfterMs = 30000;   // a candidate still waiting for frames is checked once a second after this

}  // namespace

Detector::~Detector() {
    instance_ = nullptr;
    if (exitWait_) UnregisterWaitEx(exitWait_, INVALID_HANDLE_VALUE);    // waits for a running callback
}

void Detector::SetConfig(const Config& config) {
    std::lock_guard lock(gate_);
    config_ = config;
}

void Detector::SetLists(std::vector<std::wstring> forced, std::vector<std::wstring> blocked) {
    std::lock_guard lock(gate_);
    for (auto& s : forced) s = Lower(s);
    for (auto& s : blocked) s = Lower(s);
    forced_ = std::move(forced);
    blocked_ = std::move(blocked);
    cache_.Clear();
    logged_.clear();
}

std::optional<GameInfo> Detector::Game() const {
    std::lock_guard lock(gate_);
    return game_;
}

bool Detector::Confirming() const {
    std::lock_guard lock(gate_);
    return candidate_.has_value();
}

DWORD Detector::TickInterval() const {
    std::lock_guard lock(gate_);
    return candidate_ && GetTickCount64() - candidate_->since > kSlowTickAfterMs ? 1000 : 250;
}

std::wstring Detector::Phase() const {
    std::lock_guard lock(gate_);
    if (game_) return gameFront_ ? L"game" : L"paused";
    return candidate_ ? L"confirming" : L"idle";
}

void Detector::Forget() {
    std::lock_guard lock(gate_);
    cache_.Clear();
    logged_.clear();
}

Detector::List Detector::Classify(const std::wstring& exe) const {
    if (exe.empty()) return List::None;
    auto path = Lower(exe), name = FileName(exe);
    auto match = [&](const std::vector<std::wstring>& list) {
        for (auto& item : list) if (item == path || item == name) return true;
        return false;
    };
    if (match(blocked_)) return List::Blocked;
    if (match(forced_)) return List::Forced;
    return List::None;
}

void Detector::Foreground(const WindowFacts& f) {
    Events events;
    {
        std::lock_guard lock(gate_);
        front_ = f;
        bool front = f.pid != 0 && !f.minimized;
        if (game_ && front && f.pid == game_->id.pid) {
            candidate_.reset();
            if (!gameFront_) { gameFront_ = true; events.push_back([this, g = *game_] { if (OnGameFocus) OnGameFocus(g, true); }); }
        } else {
            if (game_ && gameFront_) { gameFront_ = false; events.push_back([this, g = *game_] { if (OnGameFocus) OnGameFocus(g, false); }); }
            if (front && !game_) Evaluate(f, events);
            else if (!front) candidate_.reset();
        }
        UpdatePresent();
    }
    Run(events);
}

void Detector::Evaluate(const WindowFacts& f, Events& out) {
    if (f.pid == GetCurrentProcessId()) return;
    auto id = Identify(f.pid);
    if (!id.created) return;                                    // gone already, or not ours to see

    // Level 0: verdict cache, then the user's lists
    if (auto v = cache_.Get(id)) {
        if (v->game) Confirm(v->info, out);
        else candidate_.reset();
        return;
    }
    auto record = etw_.Process(f.pid);
    auto exe = record && !record->image.empty() ? record->image : f.exe;
    if (exe.empty()) { candidate_.reset(); return; }
    auto list = Classify(exe);
    if (list == List::Blocked) { Reject(id, exe, L"on your Never optimize list", out); return; }

    GameInfo info{ id, exe, FriendlyName(exe) };
    // Rust.exe and other launchers are renamed EAC/BattlEye bootstrappers sitting next to the game's engine files
    if (Contains(info.name, L"Anti-Cheat") || Contains(info.name, L"BattlEye")) { Reject(id, exe, L"anti-cheat bootstrapper; the game it starts is detected instead", out); return; }
    if (list == List::Forced) {
        info.forced = true;
        info.reasons = L"on your Always optimize list";
        info.antiCheat = DetectAntiCheat(exe);
        Confirm(info, out);
        return;
    }

    // Level 1: static and window heuristics
    std::wstring parent;
    if (record && record->parent)
        if (auto p = etw_.Process(record->parent)) parent = p->image;
    auto facts = Analyze(exe, parent, record ? record->modules : 0, library_.Store(exe), config_);
    auto score = xww::Evaluate(facts, f);
    if (score.rejected) { Reject(id, exe, score.reasons, out); return; }
    info.score = score.points;
    info.reasons = score.reasons;
    if (score.points < config_.scoreThreshold) {
        // not cached: the window may still go fullscreen; the agent probes it again for a minute
        Reject(id, exe, std::format(L"{} of {} points ({})", score.points, config_.scoreThreshold, score.reasons.empty() ? L"no game signs" : score.reasons), out, false);
        return;
    }

    // without ETW there is no frame rate to wait for; a clear score has to do
    if (!etw_.Running()) {
        if (score.points >= config_.scoreThreshold + 2) Confirm(info, out);
        else Reject(id, exe, std::format(L"{} points; frame rate is unavailable without ETW", score.points), out, false);
        return;
    }

    // Level 2: wait for frames
    if (!candidate_ || candidate_->info.id != id) {
        candidate_ = Candidate{ info, GetTickCount64() };
        Log(L"Detect", L"candidate {} (pid {}): {} points, {}", info.name, id.pid, score.points, score.reasons);
    } else {
        candidate_->info.score = info.score;
        candidate_->info.reasons = info.reasons;
    }
}

void Detector::Tick() {
    Events events;
    {
        std::lock_guard lock(gate_);
        if (!candidate_) return;
        auto now = GetTickCount64();
        auto& c = *candidate_;
        if (front_.pid != c.info.id.pid) { candidate_.reset(); UpdatePresent(); return; }
        // the frame window ends a second ago (ETW delivery delay); wait until it lies entirely after the candidate started
        auto age = now - c.since;
        if (age < static_cast<ULONGLONG>(config_.confirmSeconds * 1000) + 1000) return;
        double fps = etw_.Fps(c.info.id.pid, config_.confirmSeconds);
        if (fps >= config_.minFps) {
            auto info = c.info;
            info.reasons += std::format(L"{}{:.0f} fps", info.reasons.empty() ? L"" : L", ", fps);
            info.antiCheat = DetectAntiCheat(info.exe);
            Confirm(info, events);
        } else if (!c.reported && age > kSlowTickAfterMs) {
            // a launcher, a menu without a render loop or a long loading screen; keep watching while it is in front
            c.reported = true;
            auto exe = c.info.exe;
            auto reason = std::format(L"waiting for a steady frame rate ({:.0f} of {:.0f} fps)", fps, config_.minFps);
            Log(L"Detect", L"{} (pid {}): {}", c.info.name, c.info.id.pid, reason);
            events.push_back([this, exe, reason] { if (OnVerdict) OnVerdict(exe, L"undecided", reason); });
        }
        UpdatePresent();
    }
    Run(events);
}

void Detector::Confirm(GameInfo info, Events& out) {
    candidate_.reset();
    cache_.Put(info.id, { true, info });
    game_ = info;
    gameFront_ = true;
    WatchExit(info.id.pid);
    Log(L"Detect", L"game {} (pid {}){}: {}", info.name, info.id.pid, info.antiCheat.empty() ? L"" : L", anti-cheat " + info.antiCheat, info.reasons);
    out.push_back([this, info] {
        if (OnVerdict) OnVerdict(info.exe, L"game", info.reasons);
        if (OnGameStarted) OnGameStarted(info);
    });
}

void Detector::Reject(const ProcessId& id, const std::wstring& exe, const std::wstring& reason, Events& out, bool cache) {
    if (candidate_ && candidate_->info.id == id) candidate_.reset();
    if (cache) cache_.Put(id, { false, GameInfo{ id, exe } });
    auto& last = logged_[id.pid];
    if (last != reason) {
        last = reason;
        Log(L"Detect", L"{} {} (pid {}): {}", cache ? L"not a game:" : L"undecided:", FileName(exe), id.pid, reason);
    }
    out.push_back([this, exe, reason, cache] { if (OnVerdict) OnVerdict(exe, cache ? L"not a game" : L"undecided", reason); });
}

void Detector::ProcessExited(DWORD pid) {
    Events events;
    {
        std::lock_guard lock(gate_);
        cache_.EraseIf([pid](const ProcessId& id) { return id.pid == pid; });
        logged_.erase(pid);
        if (candidate_ && candidate_->info.id.pid == pid) candidate_.reset();
        if (game_ && game_->id.pid == pid) EndGame(events);
        UpdatePresent();
    }
    Run(events);
}

void Detector::EndGame(Events& out) {
    Log(L"Detect", L"game {} ended", game_->name);
    out.push_back([this, g = *game_] { if (OnGameEnded) OnGameEnded(g); });
    game_.reset();
    gameFront_ = false;
    StopWatchingExit();
}

void Detector::UpdatePresent() {
    std::vector<DWORD> watched;
    if (candidate_) watched.push_back(candidate_->info.id.pid);
    if (game_) watched.push_back(game_->id.pid);
    etw_.Watch(watched);
    etw_.EnablePresent(!watched.empty());
}

// ETW can drop a process-stop event when its buffers overflow; a wait on the game itself cannot miss the exit.
void Detector::WatchExit(DWORD pid) {
    StopWatchingExit();
    exitProcess_ = OpenLimited(pid, SYNCHRONIZE);
    if (exitProcess_ && !RegisterWaitForSingleObject(&exitWait_, exitProcess_.get(), &Detector::OnExit, reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(pid)), INFINITE, WT_EXECUTEONLYONCE))
        exitWait_ = nullptr;
}

void Detector::StopWatchingExit() {
    if (exitWait_) { UnregisterWaitEx(exitWait_, nullptr); exitWait_ = nullptr; }
    exitProcess_.reset();
}

void CALLBACK Detector::OnExit(PVOID context, BOOLEAN) {
    if (auto self = instance_.load()) self->ProcessExited(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(context)));
}

}  // namespace xww
