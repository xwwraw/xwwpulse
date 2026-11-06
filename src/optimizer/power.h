//  C-states. The active scheme is duplicated into a temporary
//  "xwwPulse Game" scheme with IDLEDISABLE = 1 (AC and DC), which is then
//  activated. The user's own scheme is never written, so the worst case after
//  a crash is a leftover copy that the next start deletes.
#pragma once

#include "core/journal.h"
#include "optimizer/steps.h"

namespace xww {

class PowerBoost {
public:
    Step Apply(bool skipOnBattery);
    void Revert();
    bool Active() const { return active_; }
    static void Recover(const journal::Entry& entry);

private:
    GUID original_{}, temp_{};
    bool active_ = false;
};

}  // namespace xww
