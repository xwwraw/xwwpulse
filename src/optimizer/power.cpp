#include "optimizer/power.h"

#include <powrprof.h>

namespace xww {

// {54533251-82BE-4824-96C1-47B60B740D00} processor power management
static constexpr GUID kSubProcessor = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
// {5D76A2CA-E8C0-402F-A133-2158492D58AD} IDLEDISABLE
static constexpr GUID kIdleDisable = { 0x5d76a2ca, 0xe8c0, 0x402f, { 0xa1, 0x33, 0x21, 0x58, 0x49, 0x2d, 0x58, 0xad } };
static constexpr wchar_t kSchemeName[] = L"xwwPulse Game";

static std::optional<GUID> ActiveScheme() {
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS || !active) return std::nullopt;
    GUID g = *active;
    LocalFree(active);
    return g;
}

Step PowerBoost::Apply(bool skipOnBattery) {
    Step step{ L"cstates", L"Processor idle states" };
    if (active_) { step.state = StepState::On; step.detail = L"cores stay out of deep C-states"; return step; }
    if (skipOnBattery && OnBattery()) { step.state = StepState::Skipped; step.detail = L"on battery"; return step; }
    auto original = ActiveScheme();
    if (!original) { step.state = StepState::Failed; step.detail = L"cannot read the active power plan"; return step; }

    GUID* copy = nullptr;
    if (PowerDuplicateScheme(nullptr, &*original, &copy) != ERROR_SUCCESS || !copy) { step.state = StepState::Failed; step.detail = L"cannot copy the power plan"; return step; }
    original_ = *original;
    temp_ = *copy;
    LocalFree(copy);
    journal::Put(L"power", { GuidText(original_), GuidText(temp_) });

    PowerWriteFriendlyName(nullptr, &temp_, nullptr, nullptr, reinterpret_cast<UCHAR*>(const_cast<wchar_t*>(kSchemeName)), sizeof(kSchemeName));
    DWORD a = PowerWriteACValueIndex(nullptr, &temp_, &kSubProcessor, &kIdleDisable, 1);
    DWORD d = PowerWriteDCValueIndex(nullptr, &temp_, &kSubProcessor, &kIdleDisable, 1);
    DWORD s = PowerSetActiveScheme(nullptr, &temp_);
    if (a != ERROR_SUCCESS || d != ERROR_SUCCESS || s != ERROR_SUCCESS) {
        Log(L"Power", L"IDLEDISABLE failed: AC {}, DC {}, activate {}", a, d, s);
        Revert();
        step.state = StepState::Failed;
        step.detail = L"Windows refused the power setting";
        return step;
    }
    active_ = true;
    Log(L"Power", L"idle states off (temporary plan {})", GuidText(temp_));
    step.state = StepState::On;
    step.detail = L"cores stay out of deep C-states";
    return step;
}

void PowerBoost::Revert() {
    if (temp_ == GUID{}) return;
    PowerSetActiveScheme(nullptr, &original_);
    PowerDeleteScheme(nullptr, &temp_);
    journal::Remove(L"power");
    if (active_) Log(L"Power", L"power plan restored");
    active_ = false;
    temp_ = {};
}

void PowerBoost::Recover(const journal::Entry& e) {
    if (e.fields.empty()) return;
    auto original = ParseGuid(e.fields[0]);
    auto temp = e.fields.size() > 1 ? ParseGuid(e.fields[1]) : std::nullopt;
    auto active = ActiveScheme();
    if (original && (!active || (temp && *active == *temp))) PowerSetActiveScheme(nullptr, &*original);
    if (temp) PowerDeleteScheme(nullptr, &*temp);
    journal::Remove(e.key);
    Log(L"Recover", L"power plan {} restored", e.fields[0]);
}

}  // namespace xww
