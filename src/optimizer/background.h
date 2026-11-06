//  Background limiter. [suspend] processes are frozen with NtSuspendProcess;
//  [throttle] processes get EcoQoS, very low I/O priority and no dynamic
//  priority boost. Each change is journaled with the values it replaces and
//  keyed by PID + creation time. The game, its launcher chain, xwwPulse
//  itself and anything the user brings to the front are never touched.
#pragma once

#include "config/config.h"
#include "core/journal.h"
#include "optimizer/steps.h"

#include <map>
#include <set>

namespace xww {

class BackgroundLimiter {
public:
    void Engage(const Config& config, const ProcessId& game, const std::set<DWORD>& protect);
    void OnProcessStart(DWORD pid, const std::wstring& image);      // during a session: newly started browsers, helpers...
    void Release(DWORD pid);                                         // the user switched to it
    void Revert();
    bool Engaged() const { return engaged_; }
    Step SuspendStep() const;
    Step ThrottleStep() const;
    static void Recover(const journal::Entry& entry);

private:
    struct Throttled { ProcessId id; ULONG control = 0, state = 0, io = 2; BOOL boostDisabled = FALSE; std::wstring name; };
    struct Suspended { ProcessId id; std::wstring name; };
    void Consider(DWORD pid, const std::wstring& name);
    bool Suspend(DWORD pid, const std::wstring& name);
    bool Throttle(DWORD pid, const std::wstring& name);
    static void Resume(const ProcessId& id);
    static void Restore(const Throttled& t);

    bool engaged_ = false, suspendOn_ = false, throttleOn_ = false;
    std::vector<std::wstring> suspendList_, throttleList_;
    std::set<DWORD> protect_, released_;
    std::map<DWORD, Suspended> suspended_;
    std::map<DWORD, Throttled> throttled_;
};

}  // namespace xww
