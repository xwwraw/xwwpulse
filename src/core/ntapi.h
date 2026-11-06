//  The few native APIs xwwPulse needs that the Windows SDK does not declare
//  (or declares only in winternl.h with missing fields). Layouts are the x64
//  ones from the public PDBs; names carry an Nt/X prefix so they never clash
//  with winternl.h. Linked against ntdll.lib.
#pragma once

#include <windows.h>
#include <cstddef>

typedef LONG NTSTATUS;
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif
#define XSTATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)

namespace xww::nt {

// ------------------------------------------------------------------ information classes

enum : ULONG {
    SystemProcessInformation = 5,
    ProcessIoPriority = 33,         // ULONG IO_PRIORITY_HINT
};

enum IoPriorityHint : ULONG {
    IoPriorityVeryLow = 0,          // background I/O; what Windows uses for the indexer
    IoPriorityLow = 1,
    IoPriorityNormal = 2,
    IoPriorityHigh = 3,             // needs SeIncreaseBasePriorityPrivilege
};

struct XUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct XClientId {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
};

struct XSystemThreadInformation {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    XClientId ClientId;
    LONG Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    ULONG WaitReason;
};
static_assert(sizeof(XSystemThreadInformation) == 0x50, "SYSTEM_THREAD_INFORMATION is 0x50 bytes on x64");

struct XSystemProcessInformation {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    XUnicodeString ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
    XSystemThreadInformation Threads[1];
};
static_assert(offsetof(XSystemProcessInformation, Threads) == 0x100, "SYSTEM_PROCESS_INFORMATION header is 0x100 bytes on x64");

// ------------------------------------------------------------------ GPU scheduling (gdi32, resolved at run time)

enum class GpuPriority : int {      // D3DKMT_SCHEDULINGPRIORITYCLASS
    Idle = 0,
    BelowNormal = 1,
    Normal = 2,
    AboveNormal = 3,
    High = 4,
    Realtime = 5,                   // needs SeIncreaseBasePriorityPrivilege; can starve DWM
};

using SetGpuPriorityFn = NTSTATUS(APIENTRY*)(HANDLE process, GpuPriority priority);
using GetGpuPriorityFn = NTSTATUS(APIENTRY*)(HANDLE process, GpuPriority* priority);

}  // namespace xww::nt

extern "C" {
NTSTATUS NTAPI NtSuspendProcess(HANDLE ProcessHandle);
NTSTATUS NTAPI NtResumeProcess(HANDLE ProcessHandle);
NTSTATUS NTAPI NtQueryInformationProcess(HANDLE ProcessHandle, ULONG ProcessInformationClass, PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength);
NTSTATUS NTAPI NtSetInformationProcess(HANDLE ProcessHandle, ULONG ProcessInformationClass, PVOID ProcessInformation, ULONG ProcessInformationLength);
NTSTATUS NTAPI NtQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength);
}
