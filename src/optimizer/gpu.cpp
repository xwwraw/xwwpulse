#include "optimizer/gpu.h"
#include "core/ntapi.h"

namespace xww {

namespace {

struct Kmt {
    nt::SetGpuPriorityFn set = nullptr;
    nt::GetGpuPriorityFn get = nullptr;
    Kmt() {
        if (HMODULE gdi = GetModuleHandleW(L"gdi32.dll") ? GetModuleHandleW(L"gdi32.dll") : LoadLibraryW(L"gdi32.dll")) {
            set = reinterpret_cast<nt::SetGpuPriorityFn>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"));
            get = reinterpret_cast<nt::GetGpuPriorityFn>(GetProcAddress(gdi, "D3DKMTGetProcessSchedulingPriorityClass"));
        }
    }
};

const Kmt& Gdi() { static Kmt kmt; return kmt; }

const wchar_t* ClassName(DWORD c) {
    switch (c) {
    case HIGH_PRIORITY_CLASS: return L"High";
    case ABOVE_NORMAL_PRIORITY_CLASS: return L"Above normal";
    case NORMAL_PRIORITY_CLASS: return L"Normal";
    case BELOW_NORMAL_PRIORITY_CLASS: return L"Below normal";
    case IDLE_PRIORITY_CLASS: return L"Idle";
    case REALTIME_PRIORITY_CLASS: return L"Realtime";
    default: return L"Unknown";
    }
}

}  // namespace

Step GpuBoost::ApplyPriority(const ProcessId& game, const std::wstring& priority) {
    Step step{ L"priority", L"CPU priority" };
    DWORD target = priority == L"high" ? HIGH_PRIORITY_CLASS : priority == L"above_normal" ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
    auto h = OpenLimited(game.pid, PROCESS_SET_INFORMATION);
    if (!h) { step.state = StepState::Failed; step.detail = L"the game denied access (anti-cheat)"; return step; }
    DWORD current = GetPriorityClass(h.get());
    if (current == target) { step.state = StepState::On; step.detail = ClassName(target); return step; }
    game_ = game;
    if (!originalClass_) originalClass_ = current;
    journal::Put(journal::Key(L"prio", game), { std::to_wstring(originalClass_) });
    if (!SetPriorityClass(h.get(), target)) {
        // anti-cheats strip the right from our handle; nothing changed, so nothing to journal
        journal::Remove(journal::Key(L"prio", game));
        originalClass_ = 0;
        step.state = StepState::Failed;
        step.detail = GetLastError() == ERROR_ACCESS_DENIED ? L"blocked by the game's anti-cheat" : std::format(L"error {}", GetLastError());
        return step;
    }
    step.state = StepState::On;
    step.detail = std::format(L"{} (was {})", ClassName(target), ClassName(originalClass_));
    return step;
}

Step GpuBoost::ApplyGpu(const ProcessId& game, const std::wstring& priority) {
    Step step{ L"gpu", L"GPU scheduling priority" };
    if (priority == L"off") { step.state = StepState::Off; step.detail = L"turned off in settings"; return step; }
    auto& kmt = Gdi();
    if (!kmt.set || !kmt.get) { step.state = StepState::Skipped; step.detail = L"not available on this Windows build"; return step; }
    auto target = priority == L"realtime" ? nt::GpuPriority::Realtime : nt::GpuPriority::High;
    if (target == nt::GpuPriority::Realtime) EnablePrivilege(SE_INC_BASE_PRIORITY_NAME);
    auto h = OpenLimited(game.pid, PROCESS_SET_INFORMATION);
    if (!h) { step.state = StepState::Failed; step.detail = L"the game denied access (anti-cheat)"; return step; }
    nt::GpuPriority current = nt::GpuPriority::Normal;
    kmt.get(h.get(), &current);
    game_ = game;
    if (originalGpu_ < 0) originalGpu_ = static_cast<int>(current);
    journal::Put(journal::Key(L"gpuprio", game), { std::to_wstring(originalGpu_) });
    NTSTATUS status = kmt.set(h.get(), target);
    if (!NT_SUCCESS(status)) {
        journal::Remove(journal::Key(L"gpuprio", game));
        originalGpu_ = -1;
        step.state = StepState::Failed;
        step.detail = static_cast<uint32_t>(status) == 0xC0000022 ? L"blocked by the game's anti-cheat" : std::format(L"D3DKMT status 0x{:08X}", static_cast<uint32_t>(status));
        return step;
    }
    step.state = StepState::On;
    step.detail = target == nt::GpuPriority::Realtime ? L"Realtime" : L"High";
    return step;
}

void GpuBoost::Revert() {
    if (!game_.valid()) return;
    if (SameProcess(game_)) {
        auto h = OpenLimited(game_.pid, PROCESS_SET_INFORMATION);
        if (h && originalClass_) SetPriorityClass(h.get(), originalClass_);
        if (h && originalGpu_ >= 0 && Gdi().set) Gdi().set(h.get(), static_cast<nt::GpuPriority>(originalGpu_));
    }
    journal::Remove(journal::Key(L"prio", game_));
    journal::Remove(journal::Key(L"gpuprio", game_));
    game_ = {};
    originalClass_ = 0;
    originalGpu_ = -1;
}

void GpuBoost::Recover(const journal::Entry& e) {
    auto id = journal::ParseProcess(e.key);
    if (!e.fields.empty() && SameProcess(id)) {
        auto h = OpenLimited(id.pid, PROCESS_SET_INFORMATION);
        DWORD value = std::wcstoul(e.fields[0].c_str(), nullptr, 10);
        if (h && e.Kind() == L"prio") SetPriorityClass(h.get(), value);
        if (h && e.Kind() == L"gpuprio" && Gdi().set) Gdi().set(h.get(), static_cast<nt::GpuPriority>(value));
    }
    journal::Remove(e.key);
}

}  // namespace xww
