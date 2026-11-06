#include "optimizer/optimizer.h"

namespace xww {

void Optimizer::SetConfig(const Config& config) {
    std::lock_guard lock(gate_);
    config_ = config;
}

void Optimizer::Engage(const GameInfo& game, const std::set<DWORD>& protect) {
    std::lock_guard lock(gate_);
    if (engaged_ && game_ && game_->id == game.id) { Resume(); return; }
    if (engaged_) Disengage();
    game_ = game;
    protect_ = protect;
    engaged_ = true;
    paused_ = false;
    if (dryRun_) { Log(L"Optimizer", L"[detect-only] would optimize {} (pid {})", game.name, game.id.pid); return; }

    // process-level changes stay for the whole session
    priorityStep_ = gpu_.ApplyPriority(game.id, config_.processPriority);
    gpuStep_ = gpu_.ApplyGpu(game.id, config_.gpuPriority);
    placementStep_ = placement_.Apply(game.id, config_.ccd, config_.cpuSetMode);
    if (placementStep_.state == StepState::On && config_.backgroundCores)
        if (auto moved = placement_.MoveBackground(config_.coreList, protect_))
            placementStep_.detail += std::format(L" · {} background app{} on the other cores", moved, moved == 1 ? L"" : L"s");
    ApplyForeground();
    Log(L"Optimizer", L"session for {}: priority {}, GPU {}, placement {}", game.name, StateText(priorityStep_.state), StateText(gpuStep_.state), placementStep_.detail);
}

// What only matters while the game is in front: C-states, background apps, DWM MMCSS.
void Optimizer::ApplyForeground() {
    if (!game_) return;
    powerStep_ = config_.idleDisable ? power_.Apply(config_.skipOnBattery) : Step{ L"cstates", L"Processor idle states", StepState::Off, L"turned off in settings" };
    background_.Engage(config_, game_->id, protect_);
    if (config_.dwmMmcss && !mmcss_) { mmcss_ = true; if (OnMmcss) OnMmcss(true); }
}

void Optimizer::RevertForeground() {
    power_.Revert();
    background_.Revert();
    if (powerStep_.state == StepState::On) powerStep_.state = StepState::Paused;
    if (mmcss_) { mmcss_ = false; if (OnMmcss) OnMmcss(false); }
}

void Optimizer::Pause() {
    std::lock_guard lock(gate_);
    if (!engaged_ || paused_ || !config_.pauseOnAltTab) return;
    paused_ = true;
    if (dryRun_) return;
    RevertForeground();
    Log(L"Optimizer", L"paused: the game is in the background");
}

void Optimizer::Resume() {
    std::lock_guard lock(gate_);
    if (!engaged_ || !paused_) return;
    paused_ = false;
    if (dryRun_) return;
    ApplyForeground();
    Log(L"Optimizer", L"resumed");
}

void Optimizer::Disengage() {
    std::lock_guard lock(gate_);
    if (!engaged_) return;
    if (!dryRun_) {
        threads_.Revert();
        RevertForeground();
        placement_.Revert();
        gpu_.Revert();
        Log(L"Optimizer", L"session ended; everything restored");
    }
    engaged_ = paused_ = false;
    game_.reset();
    powerStep_ = priorityStep_ = gpuStep_ = placementStep_ = {};
}

void Optimizer::OnProcessStart(DWORD pid, const std::wstring& image) {
    std::lock_guard lock(gate_);
    if (engaged_ && !dryRun_) placement_.OnProcessStart(pid, image);
    if (engaged_ && !paused_ && !dryRun_) background_.OnProcessStart(pid, image);
}

void Optimizer::OnForeground(DWORD pid) {
    std::lock_guard lock(gate_);
    if (engaged_ && !dryRun_ && (!game_ || pid != game_->id.pid)) background_.Release(pid);
}

bool Optimizer::WantsTick() const {
    std::lock_guard lock(gate_);
    return engaged_ && !paused_ && !dryRun_ && config_.threadBoost && game_ && (game_->antiCheat.empty() || config_.threadBoostWithAntiCheat);
}

void Optimizer::Tick() {
    std::lock_guard lock(gate_);
    if (WantsTick()) threads_.Sample(game_->id, placement_.Domain());
}

std::vector<Step> Optimizer::Steps() const {
    std::lock_guard lock(gate_);
    auto threads = threads_.Status(config_.threadBoost, game_ ? game_->antiCheat : L"", config_.threadBoostWithAntiCheat);
    auto mmcss = Step{ L"mmcss", L"DWM multimedia scheduling", config_.dwmMmcss ? (mmcss_ ? StepState::On : StepState::Paused) : StepState::Off, config_.dwmMmcss ? L"compositor threads under MMCSS" : L"turned off in settings" };
    if (!engaged_) {
        // the plan, shown on the dashboard before any game runs
        auto off = [](const wchar_t* id, const wchar_t* title, bool on, const wchar_t* what) { return Step{ id, title, StepState::Off, on ? what : L"turned off in settings" }; };
        return {
            off(L"cstates", L"Processor idle states", config_.idleDisable, L"waits for a game"),
            off(L"priority", L"CPU priority", true, L"waits for a game"),
            off(L"gpu", L"GPU scheduling priority", config_.gpuPriority != L"off", L"waits for a game"),
            off(L"ccd", L"CPU placement", config_.ccd != L"off", L"waits for a game"),
            off(L"threads", L"Game thread boost", config_.threadBoost, L"waits for a game"),
            off(L"suspend", L"Suspend background apps", config_.suspendBackground, L"waits for a game"),
            off(L"throttle", L"Throttle background apps", config_.throttleBackground, L"waits for a game"),
            off(L"mmcss", L"DWM multimedia scheduling", config_.dwmMmcss, L"waits for a game"),
        };
    }
    if (dryRun_) return { Step{ L"dry", L"Detect-only mode", StepState::Skipped, L"nothing is changed" } };
    return { powerStep_, priorityStep_, gpuStep_, placementStep_, threads, background_.SuspendStep(), background_.ThrottleStep(), mmcss };
}

void Optimizer::Recover() {
    auto entries = journal::All();
    if (entries.empty()) return;
    Log(L"Recover", L"{} journal entries from the last run", entries.size());
    for (auto& e : entries) {
        auto kind = e.Kind();
        try {
            if (kind == L"power") PowerBoost::Recover(e);
            else if (kind == L"prio" || kind == L"gpuprio") GpuBoost::Recover(e);
            else if (kind == L"cpuset" || kind == L"affinity" || kind == L"bgcpuset") CpuPlacement::Recover(e);
            else if (kind == L"thread") ThreadDispatcher::Recover(e);
            else if (kind == L"suspend" || kind == L"throttle") BackgroundLimiter::Recover(e);
            else journal::Remove(e.key);
        } catch (...) {
            journal::Remove(e.key);
        }
    }
}

}  // namespace xww
