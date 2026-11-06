#include "optimizer/threads.h"
#include "core/ntapi.h"

#include <algorithm>
#include <bit>
#include <set>
#include <tlhelp32.h>

namespace xww {

// ------------------------------------------------------------------ topology

std::vector<CacheDomain> CacheDomains() {
    std::vector<CacheDomain> domains;
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &size);
    std::vector<uint8_t> buffer(size);
    if (!GetLogicalProcessorInformationEx(RelationCache, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &size)) return domains;
    for (DWORD offset = 0; offset < size;) {
        auto info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
        if (info->Relationship == RelationCache && info->Cache.Level == 3 && info->Cache.Type != CacheInstruction)
            domains.push_back({ info->Cache.GroupMask.Group, info->Cache.GroupMask.Mask, info->Cache.CacheSize });
        offset += info->Size;
    }

    // CPU Set IDs and one logical processor per core, per domain
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<uint8_t> sets(length);
    if (length && GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(sets.data()), length, &length, GetCurrentProcess(), 0)) {
        std::set<std::pair<size_t, BYTE>> seenCores;
        for (ULONG offset = 0; offset < length;) {
            auto s = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(sets.data() + offset);
            if (s->Type == CpuSetInformation)
                for (size_t i = 0; i < domains.size(); ++i) {
                    auto& d = domains[i];
                    if (d.group == s->CpuSet.Group && (d.mask & (KAFFINITY(1) << s->CpuSet.LogicalProcessorIndex))) {
                        d.cpuSets.push_back(s->CpuSet.Id);
                        if (seenCores.insert({ i, s->CpuSet.CoreIndex }).second) d.cores.push_back(s->CpuSet.LogicalProcessorIndex);
                    }
                }
            offset += s->Size;
        }
    }
    std::sort(domains.begin(), domains.end(), [](auto& a, auto& b) { return a.group != b.group ? a.group < b.group : a.mask < b.mask; });
    return domains;
}

static std::wstring Range(const CacheDomain& d) {
    int first = std::countr_zero(static_cast<unsigned long long>(d.mask));
    int last = 63 - std::countl_zero(static_cast<unsigned long long>(d.mask));
    return std::format(L"CPU {}-{}", first, last);
}

// Intel hybrid: the CPU Sets report an efficiency class per core, and the P-cores have the higher one.
static bool SplitByEfficiency(CacheDomain& fast, CacheDomain& slow) {
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<uint8_t> sets(length);
    if (!length || !GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(sets.data()), length, &length, GetCurrentProcess(), 0)) return false;
    std::vector<const SYSTEM_CPU_SET_INFORMATION*> all;
    for (ULONG offset = 0; offset < length;) {
        auto s = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(sets.data() + offset);
        if (s->Type == CpuSetInformation) all.push_back(s);
        offset += s->Size;
    }
    if (all.empty()) return false;
    auto [low, high] = std::minmax_element(all.begin(), all.end(), [](auto a, auto b) { return a->CpuSet.EfficiencyClass < b->CpuSet.EfficiencyClass; });
    BYTE top = (*high)->CpuSet.EfficiencyClass;
    if ((*low)->CpuSet.EfficiencyClass == top) return false;
    std::set<std::pair<bool, BYTE>> seenCores;
    for (auto s : all) {
        bool performance = s->CpuSet.EfficiencyClass == top;
        auto& d = performance ? fast : slow;
        d.group = s->CpuSet.Group;
        d.mask |= KAFFINITY(1) << s->CpuSet.LogicalProcessorIndex;
        d.cpuSets.push_back(s->CpuSet.Id);
        if (seenCores.insert({ performance, s->CpuSet.CoreIndex }).second) d.cores.push_back(s->CpuSet.LogicalProcessorIndex);
    }
    return true;
}

const Topology& ProbeTopology() {
    static const Topology topology = [] {
        Topology t;
        t.domains = CacheDomains();
        if (t.domains.size() >= 2) {
            t.kind = Topology::Kind::DualCcd;
            t.vcache = std::any_of(t.domains.begin(), t.domains.end(), [&](auto& d) { return d.l3 != t.domains.front().l3; });
        } else if (SplitByEfficiency(t.fast, t.slow)) {
            t.kind = Topology::Kind::Hybrid;
        }
        return t;
    }();
    return topology;
}

std::wstring TopologyText() {
    auto& t = ProbeTopology();
    if (t.kind == Topology::Kind::Hybrid)
        return std::format(L"Hybrid: {} P-cores ({}), {} E-cores ({})", t.fast.cores.size(), Range(t.fast), t.slow.cores.size(), Range(t.slow));
    auto& domains = t.domains;
    std::wstring text = std::format(L"{} L3 domain{}", domains.size(), domains.size() == 1 ? L"" : L"s");
    for (size_t i = 0; i < domains.size(); ++i) text += std::format(L"{} {} MB ({})", i ? L"," : L":", domains[i].l3 >> 20, Range(domains[i]));
    return text;
}

// ------------------------------------------------------------------ placement

Step CpuPlacement::Apply(const ProcessId& game, const std::wstring& ccd, const std::wstring& mode) {
    Step step{ L"ccd", L"CPU placement" };
    if (ccd == L"off") { step.state = StepState::Off; step.detail = L"turned off in settings"; return step; }
    auto& topology = ProbeTopology();
    if (topology.kind == Topology::Kind::Uniform) { step.state = StepState::Skipped; step.detail = L"one L3 cache and equal cores; nothing to choose"; return step; }

    CacheDomain chosen;
    std::wstring what;
    if (topology.kind == Topology::Kind::Hybrid) {
        if (ccd == L"vcache") { step.state = StepState::Skipped; step.detail = L"no CCD has extra L3 (no 3D V-Cache)"; return step; }
        chosen = topology.fast;
        rest_ = topology.slow.cpuSets;
        what = std::format(L"{} P-cores", chosen.cores.size());
    } else {
        auto& domains = topology.domains;
        auto largest = std::max_element(domains.begin(), domains.end(), [](auto& a, auto& b) { return a.l3 < b.l3; });
        if (ccd == L"vcache" && !topology.vcache) { step.state = StepState::Skipped; step.detail = L"no CCD has extra L3 (no 3D V-Cache)"; return step; }
        // ties go to the lowest index: max_element keeps the first of equal elements
        chosen = ccd == L"ccd0" ? domains.front() : *largest;
        size_t index = std::find_if(domains.begin(), domains.end(), [&](auto& d) { return d.group == chosen.group && d.mask == chosen.mask; }) - domains.begin();
        rest_.clear();
        for (auto& d : domains) if (d.group != chosen.group || d.mask != chosen.mask) rest_.insert(rest_.end(), d.cpuSets.begin(), d.cpuSets.end());
        what = std::format(L"CCD{} · {} MB L3{}", index, chosen.l3 >> 20, topology.vcache && chosen.l3 == largest->l3 ? L" (V-Cache)" : L"");
    }
    if (chosen.group != 0 && mode == L"hard") { step.state = StepState::Skipped; step.detail = L"hard affinity only works in processor group 0"; return step; }

    auto access = mode == L"hard" ? PROCESS_SET_INFORMATION : PROCESS_SET_LIMITED_INFORMATION;
    auto h = OpenLimited(game.pid, access | PROCESS_QUERY_INFORMATION);
    if (!h) h = OpenLimited(game.pid, access);
    if (!h) { step.state = StepState::Failed; step.detail = L"the game denied access (anti-cheat)"; return step; }
    game_ = game;
    if (mode == L"hard") {
        DWORD_PTR processMask = 0, systemMask = 0;
        GetProcessAffinityMask(h.get(), &processMask, &systemMask);
        originalMask_ = processMask;
        journal::Put(journal::Key(L"affinity", game), { std::to_wstring(originalMask_) });
        if (!SetProcessAffinityMask(h.get(), static_cast<DWORD_PTR>(chosen.mask))) { step.state = StepState::Failed; step.detail = std::format(L"error {}", GetLastError()); return step; }
        hard_ = true;
    } else {
        journal::Put(journal::Key(L"cpuset", game), {});
        if (!SetProcessDefaultCpuSets(h.get(), chosen.cpuSets.data(), static_cast<ULONG>(chosen.cpuSets.size()))) { step.state = StepState::Failed; step.detail = std::format(L"error {}", GetLastError()); return step; }
    }
    domain_ = chosen;
    step.state = StepState::On;
    step.detail = std::format(L"{} · {}", what, mode == L"hard" ? L"affinity" : L"CPU Sets");
    return step;
}

// Listed background apps get the cores the game does not use, through CPU Sets only: the
// scheduler can still borrow other cores when they have nothing else to run on.
size_t CpuPlacement::MoveBackground(const std::vector<std::wstring>& names, const std::set<DWORD>& protect) {
    if (!domain_ || rest_.empty() || names.empty()) return 0;
    moveNames_ = names;
    moveProtect_ = protect;
    moveProtect_.insert(game_.pid);
    moveProtect_.insert(GetCurrentProcessId());
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return 0;
    PROCESSENTRY32W e{ sizeof(e) };
    for (BOOL ok = Process32FirstW(snap.get(), &e); ok; ok = Process32NextW(snap.get(), &e)) Move(e.th32ProcessID, Lower(e.szExeFile));
    if (!moved_.empty()) Log(L"Placement", L"{} background process{} moved to the other cores", moved_.size(), moved_.size() == 1 ? L"" : L"es");
    return moved_.size();
}

void CpuPlacement::OnProcessStart(DWORD pid, const std::wstring& image) {
    if (domain_ && !moveNames_.empty()) Move(pid, FileName(image));
}

void CpuPlacement::Move(DWORD pid, const std::wstring& name) {
    if (pid <= 4 || moved_.count(pid) || moveProtect_.count(pid)) return;
    if (std::find(moveNames_.begin(), moveNames_.end(), name) == moveNames_.end()) return;
    auto h = OpenLimited(pid, PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION);
    if (!h) return;
    ProcessId id{ pid, CreationTime(h.get()) };
    journal::Put(journal::Key(L"bgcpuset", id), { name });
    if (!SetProcessDefaultCpuSets(h.get(), rest_.data(), static_cast<ULONG>(rest_.size()))) { journal::Remove(journal::Key(L"bgcpuset", id)); return; }
    moved_[pid] = id;
}

void CpuPlacement::Revert() {
    if (!game_.valid()) return;
    if (SameProcess(game_)) {
        if (hard_) { if (auto h = OpenLimited(game_.pid, PROCESS_SET_INFORMATION)) SetProcessAffinityMask(h.get(), originalMask_); }
        else if (auto h = OpenLimited(game_.pid, PROCESS_SET_LIMITED_INFORMATION)) SetProcessDefaultCpuSets(h.get(), nullptr, 0);
    }
    journal::Remove(journal::Key(L"affinity", game_));
    journal::Remove(journal::Key(L"cpuset", game_));
    for (auto& [pid, id] : moved_) {
        if (SameProcess(id)) if (auto h = OpenLimited(pid, PROCESS_SET_LIMITED_INFORMATION)) SetProcessDefaultCpuSets(h.get(), nullptr, 0);
        journal::Remove(journal::Key(L"bgcpuset", id));
    }
    if (!moved_.empty()) Log(L"Placement", L"{} background process{} back on all cores", moved_.size(), moved_.size() == 1 ? L"" : L"es");
    moved_.clear();
    moveNames_.clear();
    moveProtect_.clear();
    rest_.clear();
    game_ = {};
    domain_.reset();
    hard_ = false;
}

void CpuPlacement::Recover(const journal::Entry& e) {
    auto id = journal::ParseProcess(e.key);
    if (SameProcess(id)) {
        if (e.Kind() == L"affinity" && !e.fields.empty()) { if (auto h = OpenLimited(id.pid, PROCESS_SET_INFORMATION)) SetProcessAffinityMask(h.get(), std::wcstoull(e.fields[0].c_str(), nullptr, 10)); }
        else if (auto h = OpenLimited(id.pid, PROCESS_SET_LIMITED_INFORMATION)) SetProcessDefaultCpuSets(h.get(), nullptr, 0);
    }
    journal::Remove(e.key);
}

// ------------------------------------------------------------------ thread dispatcher

Step ThreadDispatcher::Status(bool enabled, const std::wstring& antiCheat, bool allowWithAntiCheat) const {
    Step step{ L"threads", L"Game thread boost" };
    if (!enabled) { step.state = StepState::Off; step.detail = L"off; turn on in Optimizations"; return step; }
    if (!antiCheat.empty() && !allowWithAntiCheat) { step.state = StepState::Skipped; step.detail = antiCheat + L" is running; thread access stays off"; return step; }
    step.state = StepState::On;
    step.detail = boosted_.empty() ? L"watching for busy threads" : std::format(L"{} busy thread{} on their own cores", boosted_.size(), boosted_.size() == 1 ? L"" : L"s");
    return step;
}

static void Unthrottle(HANDLE thread, bool off) {
    THREAD_POWER_THROTTLING_STATE state{ THREAD_POWER_THROTTLING_CURRENT_VERSION };
    state.ControlMask = off ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;   // control + state 0 = never EcoQoS; control 0 = Windows decides
    state.StateMask = 0;
    SetThreadInformation(thread, ThreadPowerThrottling, &state, sizeof(state));
}

void ThreadDispatcher::Sample(const ProcessId& game, const std::optional<CacheDomain>& domain) {
    if (game_ != game) { Revert(); game_ = game; }
    ULONG needed = 0;
    if (buffer_.empty()) buffer_.resize(512 * 1024);
    NTSTATUS status;
    while ((status = NtQuerySystemInformation(nt::SystemProcessInformation, buffer_.data(), static_cast<ULONG>(buffer_.size()), &needed)) == XSTATUS_INFO_LENGTH_MISMATCH)
        buffer_.resize(needed + 64 * 1024);
    if (!NT_SUCCESS(status)) return;

    const nt::XSystemProcessInformation* p = nullptr;
    for (size_t offset = 0;;) {
        auto entry = reinterpret_cast<const nt::XSystemProcessInformation*>(buffer_.data() + offset);
        if (reinterpret_cast<ULONG_PTR>(entry->UniqueProcessId) == game.pid) { p = entry; break; }
        if (!entry->NextEntryOffset) break;
        offset += entry->NextEntryOffset;
    }
    if (!p) return;

    auto now = GetTickCount64();
    double interval = lastSample_ ? static_cast<double>(now - lastSample_) * 10000.0 : 0;   // in 100 ns
    lastSample_ = now;
    std::vector<std::pair<uint64_t, DWORD>> load;                                          // (delta, tid)
    std::map<DWORD, uint64_t> times;
    for (ULONG i = 0; i < p->NumberOfThreads; ++i) {
        auto& t = p->Threads[i];
        DWORD tid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(t.ClientId.UniqueThread));
        uint64_t total = static_cast<uint64_t>(t.KernelTime.QuadPart + t.UserTime.QuadPart);
        times[tid] = total;
        if (auto it = lastTimes_.find(tid); it != lastTimes_.end() && interval > 0 && total > it->second)
            if (static_cast<double>(total - it->second) > interval * 0.25) load.push_back({ total - it->second, tid });
    }
    lastTimes_ = std::move(times);
    std::sort(load.rbegin(), load.rend());
    if (load.size() > 4) load.resize(4);

    std::vector<BYTE> cores = domain ? domain->cores : std::vector<BYTE>{};
    WORD group = domain ? domain->group : 0;
    for (size_t i = 0; i < load.size(); ++i) {
        DWORD tid = load[i].second;
        if (boosted_.count(tid)) continue;
        Handle h(OpenThread(THREAD_SET_INFORMATION | THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid));
        if (!h) continue;
        Boosted b;
        GetThreadIdealProcessorEx(h.get(), &b.original);
        journal::Put(journal::Key(L"thread", game, tid), { std::to_wstring(b.original.Group), std::to_wstring(b.original.Number) });
        Unthrottle(h.get(), true);
        if (!cores.empty()) {
            PROCESSOR_NUMBER ideal{ group, cores[(boosted_.size()) % cores.size()], 0 };
            SetThreadIdealProcessorEx(h.get(), &ideal, nullptr);
        }
        boosted_[tid] = b;
        Log(L"Threads", L"boosted thread {} of pid {}", tid, game.pid);
    }
}

void ThreadDispatcher::Revert() {
    bool alive = game_.valid() && SameProcess(game_);
    for (auto& [tid, b] : boosted_) {
        if (alive)
            if (Handle h(OpenThread(THREAD_SET_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, tid)); h) {
                Unthrottle(h.get(), false);
                SetThreadIdealProcessorEx(h.get(), &b.original, nullptr);
            }
        journal::Remove(journal::Key(L"thread", game_, tid));
    }
    boosted_.clear();
    lastTimes_.clear();
    lastSample_ = 0;
    game_ = {};
}

void ThreadDispatcher::Recover(const journal::Entry& e) {
    auto id = journal::ParseProcess(e.key);
    DWORD tid = journal::ParseThread(e.key);
    if (tid && e.fields.size() >= 2 && SameProcess(id))
        if (Handle h(OpenThread(THREAD_SET_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, tid)); h) {
            Unthrottle(h.get(), false);
            PROCESSOR_NUMBER original{ static_cast<WORD>(std::wcstoul(e.fields[0].c_str(), nullptr, 10)), static_cast<BYTE>(std::wcstoul(e.fields[1].c_str(), nullptr, 10)), 0 };
            SetThreadIdealProcessorEx(h.get(), &original, nullptr);
        }
    journal::Remove(e.key);
}

}  // namespace xww
