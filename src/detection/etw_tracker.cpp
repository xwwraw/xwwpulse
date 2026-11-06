#include "detection/etw_tracker.h"
#include "detection/heuristics.h"
#include "core/ntapi.h"

#include <algorithm>
#include <tdh.h>
#include <tlhelp32.h>

namespace xww {

namespace {

constexpr wchar_t kSession[] = L"xwwPulse";
// {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}
constexpr GUID kKernelProcess = { 0x22fb2cd6, 0x0e7b, 0x422b, { 0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16 } };
// {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9} (the {CA11C00C-...} GUID often quoted for it does not exist)
constexpr GUID kDxgi = { 0xca11c036, 0x0102, 0x4a2d, { 0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9 } };
// {802EC45A-1E99-4B83-9920-87C98277BA9D}
constexpr GUID kDxgKrnl = { 0x802ec45a, 0x1e99, 0x4b83, { 0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d } };

constexpr ULONGLONG kKeywordProcess = 0x10, kKeywordImage = 0x40;
constexpr USHORT kProcessStart = 1, kProcessStop = 2, kImageLoad = 5;
constexpr USHORT kDxgiPresentStart = 42;
// Present_Info: one event per present call of any API, in the presenting process. Measured on Windows 11
// with an RTX 4070: it matches DXGI Present_Start one to one, while Blit_Info, Flip_Info and
// FlipMultiPlaneOverlay_Info never fire in the game's process once the driver uses hardware flip queues.
constexpr USHORT kDxgKrnlPresents[] = { 184 };
constexpr int64_t kFrameHorizonSeconds = 12;      // longer than the longest confirm_seconds plus the delivery delay

bool Prop(PEVENT_RECORD e, const wchar_t* name, void* out, ULONG size) {
    PROPERTY_DATA_DESCRIPTOR d{ reinterpret_cast<ULONGLONG>(name), ULONG_MAX, 0 };
    return TdhGetProperty(e, 0, nullptr, 1, &d, size, static_cast<PBYTE>(out)) == ERROR_SUCCESS;
}

std::wstring PropText(PEVENT_RECORD e, const wchar_t* name) {
    PROPERTY_DATA_DESCRIPTOR d{ reinterpret_cast<ULONGLONG>(name), ULONG_MAX, 0 };
    ULONG size = 0;
    if (TdhGetPropertySize(e, 0, nullptr, 1, &d, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) return {};
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (TdhGetProperty(e, 0, nullptr, 1, &d, size, reinterpret_cast<PBYTE>(s.data())) != ERROR_SUCCESS) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

// EnableTraceEx2 with a provider-side event-ID filter, so unwanted events are never even written.
ULONG Enable(TRACEHANDLE session, const GUID& provider, bool on, ULONGLONG keywords, UCHAR level, const USHORT* ids, USHORT count) {
    if (!on) return EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
    if (!count) return EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, level, keywords, 0, 0, nullptr);
    std::vector<uint8_t> filter(offsetof(EVENT_FILTER_EVENT_ID, Events) + sizeof(USHORT) * count);
    auto f = reinterpret_cast<EVENT_FILTER_EVENT_ID*>(filter.data());
    f->FilterIn = TRUE;
    f->Count = count;
    for (USHORT i = 0; i < count; ++i) f->Events[i] = ids[i];
    EVENT_FILTER_DESCRIPTOR desc{ reinterpret_cast<ULONGLONG>(filter.data()), static_cast<ULONG>(filter.size()), EVENT_FILTER_TYPE_EVENT_ID };
    ENABLE_TRACE_PARAMETERS params{};
    params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    params.EnableFilterDesc = &desc;
    params.FilterDescCount = 1;
    return EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, level, keywords, 0, 0, &params);
}

struct BasicInformation {          // PROCESS_BASIC_INFORMATION
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
};

}  // namespace

bool EtwTracker::Start() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFrequency_ = f.QuadPart;

    // drive letters for the \Device\HarddiskVolumeN paths the kernel reports
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        wchar_t drive[3] = { letter, L':', 0 }, target[MAX_PATH];
        if (QueryDosDeviceW(drive, target, MAX_PATH)) devices_.emplace_back(target, drive);
    }
    if (Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)); snap) {
        PROCESSENTRY32W e{ sizeof(e) };
        for (BOOL ok = Process32FirstW(snap.get(), &e); ok; ok = Process32NextW(snap.get(), &e))
            if (_wcsicmp(e.szExeFile, L"dwm.exe") == 0) dwmPid_ = e.th32ProcessID;
    }

    properties_.assign(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSession) + 16, 0);
    auto props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties_.data());
    auto prepare = [&] {
        std::fill(properties_.begin(), properties_.end(), uint8_t(0));
        props->Wnode.BufferSize = static_cast<ULONG>(properties_.size());
        props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;                 // QPC timestamps
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->BufferSize = 64;                         // KB
        props->MinimumBuffers = 4;
        props->MaximumBuffers = 32;
        props->FlushTimer = 1;                          // s
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    };
    prepare();
    ULONG status = StartTraceW(&session_, kSession, props);
    if (status == ERROR_ALREADY_EXISTS) {                // left over from a crash
        prepare();
        ControlTraceW(0, kSession, props, EVENT_TRACE_CONTROL_STOP);
        prepare();
        status = StartTraceW(&session_, kSession, props);
    }
    if (status != ERROR_SUCCESS) { Log(L"ETW", L"StartTrace failed ({})", status); return false; }

    const USHORT processIds[] = { kProcessStart, kProcessStop, kImageLoad };
    status = Enable(session_, kKernelProcess, true, kKeywordProcess | kKeywordImage, TRACE_LEVEL_INFORMATION, processIds, 3);
    if (status != ERROR_SUCCESS) Log(L"ETW", L"Kernel-Process provider failed ({})", status);

    EVENT_TRACE_LOGFILEW log{};
    log.LoggerName = const_cast<LPWSTR>(kSession);
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback = &EtwTracker::OnEvent;
    log.Context = this;
    consumer_ = OpenTraceW(&log);
    if (consumer_ == INVALID_PROCESSTRACE_HANDLE) { Log(L"ETW", L"OpenTrace failed ({})", GetLastError()); Stop(); return false; }
    thread_ = std::thread([this] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        TRACEHANDLE handle = consumer_;
        ProcessTrace(&handle, 1, nullptr, nullptr);
    });
    Log(L"ETW", L"session started");
    return true;
}

void EtwTracker::Stop() {
    if (session_) {
        auto props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties_.data());
        props->Wnode.BufferSize = static_cast<ULONG>(properties_.size());
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(session_, nullptr, props, EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }
    if (consumer_ != INVALID_PROCESSTRACE_HANDLE) { CloseTrace(consumer_); consumer_ = INVALID_PROCESSTRACE_HANDLE; }
    if (thread_.joinable()) thread_.join();
    presentOn_ = false;
}

void EtwTracker::EnablePresent(bool on) {
    if (!session_ || presentOn_ == on) return;
    presentOn_ = on;
    const USHORT dxgiIds[] = { kDxgiPresentStart };
    auto a = Enable(session_, kDxgi, on, ~0ULL, TRACE_LEVEL_INFORMATION, dxgiIds, 1);
    auto b = Enable(session_, kDxgKrnl, on, ~0ULL, TRACE_LEVEL_INFORMATION, kDxgKrnlPresents, static_cast<USHORT>(std::size(kDxgKrnlPresents)));
    if (a != ERROR_SUCCESS || b != ERROR_SUCCESS) Log(L"ETW", L"present providers {}: DXGI {}, DxgKrnl {}", on ? L"on" : L"off", a, b);
    if (!on) { std::lock_guard lock(gate_); frames_.clear(); }
}

void WINAPI EtwTracker::OnEvent(PEVENT_RECORD record) {
    static_cast<EtwTracker*>(record->UserContext)->Dispatch(record);
}

void EtwTracker::Dispatch(PEVENT_RECORD e) {
    ++events_;
    auto& h = e->EventHeader;
    USHORT id = h.EventDescriptor.Id;

    if (IsEqualGUID(h.ProviderId, kDxgi)) { ++dxgiEvents_; if (diagnostics_) { std::lock_guard lock(gate_); ++kernelIds_[static_cast<USHORT>(10000 + id)]; ++dxgiPids_[h.ProcessId]; } if (id == kDxgiPresentStart) Present(h.ProcessId, h.TimeStamp.QuadPart, false); return; }
    if (IsEqualGUID(h.ProviderId, kDxgKrnl)) {
        ++kernelEvents_;
        if (diagnostics_) { std::lock_guard lock(gate_); ++kernelIds_[id]; }
        Present(h.ProcessId, h.TimeStamp.QuadPart, true);
        return;
    }
    if (!IsEqualGUID(h.ProviderId, kKernelProcess)) return;

    DWORD pid = 0;
    if (!Prop(e, L"ProcessID", &pid, sizeof(pid))) return;
    if (id == kProcessStart) {
        ProcessRecord r;
        Prop(e, L"ParentProcessID", &r.parent, sizeof(r.parent));
        r.image = Win32Path(PropText(e, L"ImageName"));
        if (FileName(r.image) == L"dwm.exe") dwmPid_ = pid;
        {
            std::lock_guard lock(gate_);
            processes_[pid] = r;
        }
        if (OnProcessStart) OnProcessStart(pid, r);
    } else if (id == kProcessStop) {
        {
            std::lock_guard lock(gate_);
            processes_.erase(pid);
            frames_.erase(pid);
        }
        if (OnProcessStop) OnProcessStop(pid);
    } else if (id == kImageLoad) {
        auto flags = ClassifyModule(PropText(e, L"ImageName"));
        if (!flags) return;
        std::lock_guard lock(gate_);
        processes_[pid].modules |= flags;
    }
}

void EtwTracker::Watch(std::vector<DWORD> pids) {
    std::lock_guard lock(gate_);
    watched_ = std::move(pids);
    std::erase_if(frames_, [&](auto& f) { return std::find(watched_.begin(), watched_.end(), f.first) == watched_.end(); });
}

void EtwTracker::Present(DWORD pid, int64_t qpc, bool kernel) {
    if (pid == 0 || pid == 4 || pid == dwmPid_) return;
    std::lock_guard lock(gate_);
    if (!diagnostics_ && std::find(watched_.begin(), watched_.end(), pid) == watched_.end()) return;
    auto& f = frames_[pid];
    auto& ring = kernel ? f.kernel : f.dxgi;
    ring.push_back(qpc);
    int64_t horizon = qpc - qpcFrequency_ * kFrameHorizonSeconds;
    while (!ring.empty() && ring.front() < horizon) ring.pop_front();
}

double EtwTracker::Fps(DWORD pid, double seconds) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // events reach the consumer up to a flush interval late, so measure the window ending 1 s ago
    int64_t end = now.QuadPart - qpcFrequency_;
    int64_t start = end - static_cast<int64_t>(seconds * qpcFrequency_);
    std::lock_guard lock(gate_);
    auto it = frames_.find(pid);
    if (it == frames_.end()) return 0;
    auto count = [&](const std::deque<int64_t>& ring) {
        size_t n = 0;
        for (auto t : ring) if (t >= start && t < end) ++n;
        return n;
    };
    // both count present calls: DXGI for D3D10-12, DxgKrnl for every API including Vulkan and OpenGL. Never the sum.
    return static_cast<double>(std::max(count(it->second.kernel), count(it->second.dxgi))) / seconds;
}

std::vector<std::pair<DWORD, double>> EtwTracker::TopFps(size_t count) {
    std::vector<DWORD> pids;
    {
        std::lock_guard lock(gate_);
        for (auto& [pid, f] : frames_) pids.push_back(pid);
    }
    std::vector<std::pair<DWORD, double>> top;
    for (auto pid : pids) if (double fps = Fps(pid, 1.0); fps > 0) top.push_back({ pid, fps });
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (top.size() > count) top.resize(count);
    return top;
}

std::optional<ProcessRecord> EtwTracker::Process(DWORD pid) {
    {
        std::lock_guard lock(gate_);
        if (auto it = processes_.find(pid); it != processes_.end() && !it->second.image.empty()) return it->second;
    }
    // started before the session: fill in from a limited handle (the same right Task Manager uses)
    auto h = OpenLimited(pid);
    if (!h) return std::nullopt;
    ProcessRecord r;
    r.image = ImagePath(pid);
    BasicInformation info{};
    if (NT_SUCCESS(NtQueryInformationProcess(h.get(), 0, &info, sizeof(info), nullptr))) r.parent = static_cast<DWORD>(info.InheritedFromUniqueProcessId);
    std::lock_guard lock(gate_);
    auto& slot = processes_[pid];
    slot.parent = r.parent;
    slot.image = r.image;
    return slot;
}

std::wstring EtwTracker::Win32Path(const std::wstring& path) {
    for (auto& [device, drive] : devices_)
        if (path.size() > device.size() && _wcsnicmp(path.c_str(), device.c_str(), device.size()) == 0 && path[device.size()] == L'\\')
            return drive + path.substr(device.size());
    return path;
}

}  // namespace xww
