//  One line per optimization for the dashboard: what it is, whether it is
//  applied, and why not when it is not.
#pragma once

#include "core/common.h"

namespace xww {

enum class StepState { Off, On, Paused, Skipped, Failed };

struct Step {
    std::wstring id, title;
    StepState state = StepState::Off;
    std::wstring detail;
};

inline const wchar_t* StateText(StepState s) {
    switch (s) {
    case StepState::On: return L"on";
    case StepState::Paused: return L"paused";
    case StepState::Skipped: return L"skipped";
    case StepState::Failed: return L"failed";
    default: return L"off";
    }
}

inline StepState StateFrom(std::wstring_view s) {
    if (s == L"on") return StepState::On;
    if (s == L"paused") return StepState::Paused;
    if (s == L"skipped") return StepState::Skipped;
    if (s == L"failed") return StepState::Failed;
    return StepState::Off;
}

// "id~state~title~detail|..." (the IPC escaping already covers the outer message)
inline std::wstring EncodeSteps(const std::vector<Step>& steps) {
    std::wstring out;
    for (auto& s : steps) out += std::format(L"{}{}~{}~{}~{}", out.empty() ? L"" : L"|", s.id, StateText(s.state), s.title, s.detail);
    return out;
}

inline std::vector<Step> DecodeSteps(const std::wstring& text) {
    std::vector<Step> steps;
    if (text.empty()) return steps;
    for (auto& item : Split(text, L'|')) {
        auto f = Split(item, L'~');
        if (f.size() >= 4) steps.push_back({ f[0], f[2], StateFrom(f[1]), f[3] });
    }
    return steps;
}

}  // namespace xww
