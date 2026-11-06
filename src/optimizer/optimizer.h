//  Runs a game session: applies every optimization in order, pauses the
//  ones that matter only in the foreground on Alt+Tab, and undoes all of it
//  when the game ends, xwwPulse stops or anything crashes. Recover() replays
//  the journal left by a previous run.
#pragma once

#include "detection/detector.h"
#include "optimizer/background.h"
#include "optimizer/gpu.h"
#include "optimizer/power.h"
#include "optimizer/threads.h"

#include <mutex>

namespace xww {

class Optimizer {
public:
    std::function<void(bool on)> OnMmcss;       // forwarded to the agent

    void SetConfig(const Config& config);
    void SetDryRun(bool on) { dryRun_ = on; }   // --detect-only: log what would happen, change nothing
    void Engage(const GameInfo& game, const std::set<DWORD>& protect);
    void Pause();                               // game left the foreground
    void Resume();                              // game back in front
    void Disengage();                           // game ended, xwwPulse disabled or stopping
    void OnProcessStart(DWORD pid, const std::wstring& image);
    void OnForeground(DWORD pid);               // releases a limited background app the user switched to
    void Tick();                                // ~2 s while the game is in front (thread dispatcher)
    bool WantsTick() const;
    bool Engaged() const { return engaged_; }
    bool Paused() const { return paused_; }
    std::vector<Step> Steps() const;

    static void Recover();

private:
    void ApplyForeground();
    void RevertForeground();

    mutable std::recursive_mutex gate_;
    Config config_ = Config::Defaults();
    std::optional<GameInfo> game_;
    std::set<DWORD> protect_;
    bool engaged_ = false, paused_ = false, dryRun_ = false, mmcss_ = false;
    PowerBoost power_;
    GpuBoost gpu_;
    CpuPlacement placement_;
    ThreadDispatcher threads_;
    BackgroundLimiter background_;
    Step powerStep_, priorityStep_, gpuStep_, placementStep_;
};

}  // namespace xww
