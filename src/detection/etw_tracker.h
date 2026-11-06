//  One real-time ETW session ("xwwPulse") for everything the service needs
//  to know without opening other processes:
//    Microsoft-Windows-Kernel-Process   process start/stop, image loads (always on)
//    Microsoft-Windows-DXGI             Present_Start (on demand)
//    Microsoft-Windows-DxgKrnl          Present_Info, event-ID filtered (on demand;
//                                        catches Vulkan and OpenGL, which bypass DXGI)
//  Present tracking is switched on only while a candidate is being confirmed
//  or a game is running, and frames are kept only for the processes being
//  watched, so an idle desktop costs nothing.
#pragma once

#include "core/common.h"

#include <evntrace.h>
#include <evntcons.h>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace xww {

struct ProcessRecord {
    DWORD parent = 0;
    std::wstring image;             // Win32 path
    uint32_t modules = 0;           // ModuleFlags seen in ImageLoad events
};

class EtwTracker {
public:
    std::function<void(DWORD pid, const ProcessRecord&)> OnProcessStart;
    std::function<void(DWORD pid)> OnProcessStop;

    bool Start();
    void Stop();
    void EnablePresent(bool on);
    bool PresentEnabled() const { return presentOn_; }
    bool Running() const { return session_ != 0; }
    void Watch(std::vector<DWORD> pids);                  // frames are recorded for these processes only

    double Fps(DWORD pid, double seconds = 2.0);          // presents per second over the last `seconds`
    std::optional<ProcessRecord> Process(DWORD pid);      // from ETW; processes older than the session are filled lazily
    std::vector<std::pair<DWORD, double>> TopFps(size_t count);   // busiest presenters, for --etw-test
    uint64_t Events() const { return events_; }
    uint64_t DxgiEvents() const { return dxgiEvents_; }
    uint64_t KernelPresentEvents() const { return kernelEvents_; }
    std::map<USHORT, uint64_t> KernelEventIds() { std::lock_guard lock(gate_); return kernelIds_; }   // --etw-test diagnostics
    std::map<DWORD, uint64_t> DxgiPids() { std::lock_guard lock(gate_); return dxgiPids_; }
    void SetDiagnostics(bool on) { diagnostics_ = on; }              // per-ID and per-PID counts; off in the service

    ~EtwTracker() { Stop(); }

private:
    static void WINAPI OnEvent(PEVENT_RECORD record);
    void Dispatch(PEVENT_RECORD record);
    void Present(DWORD pid, int64_t qpc, bool kernel);
    std::wstring Win32Path(const std::wstring& devicePath);

    TRACEHANDLE session_ = 0, consumer_ = INVALID_PROCESSTRACE_HANDLE;
    std::vector<uint8_t> properties_;
    std::thread thread_;
    std::atomic<bool> presentOn_{ false };
    std::atomic<uint64_t> events_{ 0 }, dxgiEvents_{ 0 }, kernelEvents_{ 0 };
    std::atomic<bool> diagnostics_{ false };
    int64_t qpcFrequency_ = 1;
    DWORD dwmPid_ = 0;

    std::mutex gate_;
    struct Frames { std::deque<int64_t> dxgi, kernel; };
    std::unordered_map<DWORD, Frames> frames_;
    std::unordered_map<DWORD, ProcessRecord> processes_;
    std::vector<DWORD> watched_;
    std::map<USHORT, uint64_t> kernelIds_;
    std::map<DWORD, uint64_t> dxgiPids_;
    std::vector<std::pair<std::wstring, std::wstring>> devices_;   // \Device\HarddiskVolume3 -> C:
};

}  // namespace xww
