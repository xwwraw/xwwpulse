#include "optimizer/background.h"
#include "core/ntapi.h"

#include <tlhelp32.h>

namespace xww {

void BackgroundLimiter::Engage(const Config& config, const ProcessId& game, const std::set<DWORD>& protect) {
    if (engaged_) return;
    engaged_ = true;
    suspendOn_ = config.suspendBackground;
    throttleOn_ = config.throttleBackground;
    suspendList_ = config.suspendList;
    throttleList_ = config.throttleList;
    protect_ = protect;
    protect_.insert(game.pid);
    protect_.insert(GetCurrentProcessId());
    released_.clear();
    if (!suspendOn_ && !throttleOn_) return;

    // one snapshot when the session starts; later processes arrive through ETW
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return;
    std::map<DWORD, std::pair<DWORD, std::wstring>> all;    // pid -> (parent, name)
    PROCESSENTRY32W e{ sizeof(e) };
    for (BOOL ok = Process32FirstW(snap.get(), &e); ok; ok = Process32NextW(snap.get(), &e)) all[e.th32ProcessID] = { e.th32ParentProcessID, Lower(e.szExeFile) };
    // the game's ancestors (Steam, a launcher) keep running normally
    for (DWORD pid = game.pid, hops = 0; hops < 8; ++hops) {
        auto it = all.find(pid);
        if (it == all.end()) break;
        pid = it->second.first;
        protect_.insert(pid);
    }
    for (auto& [pid, info] : all) Consider(pid, info.second);
    Log(L"Background", L"suspended {}, throttled {}", suspended_.size(), throttled_.size());
}

void BackgroundLimiter::OnProcessStart(DWORD pid, const std::wstring& image) {
    if (engaged_) Consider(pid, FileName(image));
}

void BackgroundLimiter::Consider(DWORD pid, const std::wstring& name) {
    if (pid <= 4 || protect_.count(pid) || released_.count(pid)) return;
    auto in = [&](const std::vector<std::wstring>& list) { return std::find(list.begin(), list.end(), name) != list.end(); };
    if (suspendOn_ && in(suspendList_)) Suspend(pid, name);
    else if (throttleOn_ && in(throttleList_)) Throttle(pid, name);
}

bool BackgroundLimiter::Suspend(DWORD pid, const std::wstring& name) {
    if (suspended_.count(pid)) return true;
    auto h = OpenLimited(pid, PROCESS_SUSPEND_RESUME);
    if (!h) return false;
    ProcessId id{ pid, CreationTime(h.get()) };
    journal::Put(journal::Key(L"suspend", id), { name });
    if (!NT_SUCCESS(NtSuspendProcess(h.get()))) { journal::Remove(journal::Key(L"suspend", id)); return false; }
    suspended_[pid] = { id, name };
    return true;
}

bool BackgroundLimiter::Throttle(DWORD pid, const std::wstring& name) {
    if (throttled_.count(pid)) return true;
    auto h = OpenLimited(pid, PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION);
    if (!h) return false;
    Throttled t{ { pid, CreationTime(h.get()) } };
    t.name = name;
    PROCESS_POWER_THROTTLING_STATE power{ PROCESS_POWER_THROTTLING_CURRENT_VERSION };
    if (GetProcessInformation(h.get(), ProcessPowerThrottling, &power, sizeof(power))) { t.control = power.ControlMask; t.state = power.StateMask; }
    NtQueryInformationProcess(h.get(), nt::ProcessIoPriority, &t.io, sizeof(t.io), nullptr);
    GetProcessPriorityBoost(h.get(), &t.boostDisabled);
    journal::Put(journal::Key(L"throttle", t.id), { std::to_wstring(t.control), std::to_wstring(t.state), std::to_wstring(t.io), std::to_wstring(t.boostDisabled), name });

    power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;    // EcoQoS
    power.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(h.get(), ProcessPowerThrottling, &power, sizeof(power));
    ULONG io = nt::IoPriorityVeryLow;
    NtSetInformationProcess(h.get(), nt::ProcessIoPriority, &io, sizeof(io));
    SetProcessPriorityBoost(h.get(), TRUE);
    throttled_[pid] = t;
    return true;
}

void BackgroundLimiter::Resume(const ProcessId& id) {
    if (!SameProcess(id)) return;
    if (auto h = OpenLimited(id.pid, PROCESS_SUSPEND_RESUME)) NtResumeProcess(h.get());
}

void BackgroundLimiter::Restore(const Throttled& t) {
    if (!SameProcess(t.id)) return;
    auto h = OpenLimited(t.id.pid, PROCESS_SET_INFORMATION);
    if (!h) return;
    PROCESS_POWER_THROTTLING_STATE power{ PROCESS_POWER_THROTTLING_CURRENT_VERSION, t.control, t.state };
    SetProcessInformation(h.get(), ProcessPowerThrottling, &power, sizeof(power));
    ULONG io = t.io;
    NtSetInformationProcess(h.get(), nt::ProcessIoPriority, &io, sizeof(io));
    SetProcessPriorityBoost(h.get(), t.boostDisabled);
}

void BackgroundLimiter::Release(DWORD pid) {
    if (!engaged_) return;
    released_.insert(pid);
    if (auto it = suspended_.find(pid); it != suspended_.end()) {
        Resume(it->second.id);
        journal::Remove(journal::Key(L"suspend", it->second.id));
        Log(L"Background", L"{} is in front; resumed", it->second.name);
        suspended_.erase(it);
    }
    if (auto it = throttled_.find(pid); it != throttled_.end()) {
        Restore(it->second);
        journal::Remove(journal::Key(L"throttle", it->second.id));
        throttled_.erase(it);
    }
}

void BackgroundLimiter::Revert() {
    for (auto& [pid, s] : suspended_) { Resume(s.id); journal::Remove(journal::Key(L"suspend", s.id)); }
    for (auto& [pid, t] : throttled_) { Restore(t); journal::Remove(journal::Key(L"throttle", t.id)); }
    if (engaged_ && (!suspended_.empty() || !throttled_.empty())) Log(L"Background", L"resumed {}, unthrottled {}", suspended_.size(), throttled_.size());
    suspended_.clear();
    throttled_.clear();
    released_.clear();
    engaged_ = false;
}

Step BackgroundLimiter::SuspendStep() const {
    Step s{ L"suspend", L"Suspend background apps" };
    if (!suspendOn_) { s.state = StepState::Off; s.detail = L"turned off in settings"; return s; }
    s.state = engaged_ ? StepState::On : StepState::Paused;
    std::wstring names;
    for (auto& [pid, p] : suspended_) if (names.find(p.name) == std::wstring::npos) names += (names.empty() ? L"" : L", ") + p.name;
    s.detail = suspended_.empty() ? L"none of the listed apps is running" : names;
    return s;
}

Step BackgroundLimiter::ThrottleStep() const {
    Step s{ L"throttle", L"Throttle background apps" };
    if (!throttleOn_) { s.state = StepState::Off; s.detail = L"turned off in settings"; return s; }
    s.state = engaged_ ? StepState::On : StepState::Paused;
    s.detail = throttled_.empty() ? L"none of the listed apps is running" : std::format(L"{} process{} on EcoQoS and very low I/O", throttled_.size(), throttled_.size() == 1 ? L"" : L"es");
    return s;
}

void BackgroundLimiter::Recover(const journal::Entry& e) {
    auto id = journal::ParseProcess(e.key);
    if (e.Kind() == L"suspend") Resume(id);
    else if (e.fields.size() >= 4) {
        Throttled t{ id, std::wcstoul(e.fields[0].c_str(), nullptr, 10), std::wcstoul(e.fields[1].c_str(), nullptr, 10), std::wcstoul(e.fields[2].c_str(), nullptr, 10), e.fields[3] == L"1" };
        Restore(t);
    }
    journal::Remove(e.key);
}

}  // namespace xww
