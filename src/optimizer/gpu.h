//  The game's CPU priority class and its GPU scheduling priority
//  (gdi32!D3DKMTSetProcessSchedulingPriorityClass). Both only need
//  PROCESS_SET_INFORMATION, the right Task Manager uses. DWM's MMCSS switch
//  lives in the agent, because DwmEnableMMCSS only lasts as long as the
//  process that calls it, and that process must be in the user's session.
#pragma once

#include "core/journal.h"
#include "optimizer/steps.h"

namespace xww {

class GpuBoost {
public:
    Step ApplyPriority(const ProcessId& game, const std::wstring& priority);   // normal | above_normal | high
    Step ApplyGpu(const ProcessId& game, const std::wstring& priority);        // off | high | realtime
    void Revert();
    static void Recover(const journal::Entry& entry);

private:
    ProcessId game_;
    DWORD originalClass_ = 0;
    int originalGpu_ = -1;
};

}  // namespace xww
