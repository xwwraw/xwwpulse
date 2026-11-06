//  Write-ahead journal of every system change, in
//  HKLM\SOFTWARE\xwwPulse\Journal. An entry is written (and flushed)
//  before the change is made and removed after it is undone, so a crash,
//  a kill or a power cut leaves enough behind to restore on the next start.
//  Value name: "<kind>:<pid>:<created>[:<tid>]" or "<kind>"; data: "a|b|c".
#pragma once

#include "core/common.h"

namespace xww::journal {

struct Entry {
    std::wstring key;
    std::vector<std::wstring> fields;
    std::wstring Kind() const { return key.substr(0, key.find(L':')); }
};

void Put(const std::wstring& key, const std::vector<std::wstring>& fields);
void Remove(const std::wstring& key);
std::vector<Entry> All();
void Clear();

inline std::wstring Key(const wchar_t* kind, const ProcessId& id) { return std::format(L"{}:{}:{}", kind, id.pid, id.created); }
inline std::wstring Key(const wchar_t* kind, const ProcessId& id, DWORD tid) { return std::format(L"{}:{}:{}:{}", kind, id.pid, id.created, tid); }
ProcessId ParseProcess(const std::wstring& key);       // "<kind>:<pid>:<created>..." -> {pid, created}
DWORD ParseThread(const std::wstring& key);            // the 4th part, or 0

}  // namespace xww::journal
