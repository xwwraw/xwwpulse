//  CPU placement and the thread dispatcher.
//  Topology: logical processors grouped by L3 cache. On a Ryzen with two
//  CCDs that gives one group per CCD; the 3D V-Cache CCD is the one with the
//  larger L3, whatever its index.
//  On Intel hybrid CPUs (one L3, two efficiency classes) the fast domain is
//  the P-cores. The probe runs once; on a CPU with one L3 and equal cores
//  placement switches itself off.
//  Placement: CPU Sets (soft; the scheduler may still use other cores under
//  pressure) or, on request, a hard affinity mask. Listed background apps
//  (Discord, browsers, OBS...) get the remaining cores as their CPU Sets for
//  the session, so they stop competing for the game's cache.
//  Dispatcher (opt-in, never with an anti-cheat unless allowed): every 2 s,
//  one NtQuerySystemInformation snapshot finds the busiest game threads; the
//  top four lose power throttling and get distinct physical cores as their
//  ideal processor.
#pragma once

#include "core/journal.h"
#include "optimizer/steps.h"

#include <map>
#include <set>

namespace xww {

struct CacheDomain {
    WORD group = 0;
    KAFFINITY mask = 0;
    DWORD l3 = 0;                   // bytes
    std::vector<ULONG> cpuSets;     // CPU Set IDs in this domain
    std::vector<BYTE> cores;        // one logical processor per physical core (for ideal processors)
};

std::vector<CacheDomain> CacheDomains();

struct Topology {
    enum class Kind { Uniform, DualCcd, Hybrid } kind = Kind::Uniform;
    bool vcache = false;                // DualCcd: one CCD has more L3
    std::vector<CacheDomain> domains;   // DualCcd: one per L3, sorted by processor number
    CacheDomain fast, slow;             // Hybrid: P-cores and E-cores
};
const Topology& ProbeTopology();   // computed on first use, then cached
std::wstring TopologyText();       // "2 L3 domains: 96 MB (CPU 0-15), 32 MB (CPU 16-31)" or "Hybrid: 8 P-cores (CPU 0-15), ..."

class CpuPlacement {
public:
    Step Apply(const ProcessId& game, const std::wstring& ccd, const std::wstring& mode);
    size_t MoveBackground(const std::vector<std::wstring>& names, const std::set<DWORD>& protect);  // after Apply; returns how many moved
    void OnProcessStart(DWORD pid, const std::wstring& image);
    void Revert();
    std::optional<CacheDomain> Domain() const { return domain_; }
    static void Recover(const journal::Entry& entry);

private:
    void Move(DWORD pid, const std::wstring& name);

    ProcessId game_;
    std::optional<CacheDomain> domain_;
    bool hard_ = false;
    DWORD_PTR originalMask_ = 0;
    std::vector<ULONG> rest_;                   // CPU Set IDs outside the game's domain
    std::vector<std::wstring> moveNames_;
    std::set<DWORD> moveProtect_;
    std::map<DWORD, ProcessId> moved_;
};

class ThreadDispatcher {
public:
    Step Status(bool enabled, const std::wstring& antiCheat, bool allowWithAntiCheat) const;
    void Sample(const ProcessId& game, const std::optional<CacheDomain>& domain);   // call every ~2 s while the game is in front
    void Revert();
    static void Recover(const journal::Entry& entry);

private:
    struct Boosted { PROCESSOR_NUMBER original{}; };
    ProcessId game_;
    std::map<DWORD, uint64_t> lastTimes_;   // tid -> kernel + user 100 ns
    ULONGLONG lastSample_ = 0;
    std::map<DWORD, Boosted> boosted_;
    std::vector<uint8_t> buffer_;
};

}  // namespace xww
