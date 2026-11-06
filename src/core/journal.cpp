#include "core/journal.h"

#include <mutex>

namespace xww::journal {

static std::mutex gate;
static const std::wstring kJournal = std::wstring(kStateKey) + L"\\Journal";

void Put(const std::wstring& key, const std::vector<std::wstring>& fields) {
    std::wstring data;
    for (size_t i = 0; i < fields.size(); ++i) data += (i ? L"|" : L"") + fields[i];
    std::lock_guard lock(gate);
    RegKey k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kJournal.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, k.put(), nullptr) != ERROR_SUCCESS) {
        Log(L"Journal", L"cannot open the journal key; {} will not survive a crash", key);
        return;
    }
    RegSetValueExW(k.get(), key.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()), static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegFlushKey(k.get());   // rare and important: the entry must be on disk before the change
}

void Remove(const std::wstring& key) {
    std::lock_guard lock(gate);
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kJournal.c_str(), key.c_str());
}

std::vector<Entry> All() {
    std::lock_guard lock(gate);
    std::vector<Entry> entries;
    for (auto& name : RegValueNames(HKEY_LOCAL_MACHINE, kJournal))
        if (auto data = RegReadString(HKEY_LOCAL_MACHINE, kJournal, name.c_str()))
            entries.push_back({ name, Split(*data, L'|') });
    return entries;
}

void Clear() {
    std::lock_guard lock(gate);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kJournal.c_str());
}

ProcessId ParseProcess(const std::wstring& key) {
    auto parts = Split(key, L':');
    if (parts.size() < 3) return {};
    return { static_cast<DWORD>(std::wcstoul(parts[1].c_str(), nullptr, 10)), std::wcstoull(parts[2].c_str(), nullptr, 10) };
}

DWORD ParseThread(const std::wstring& key) {
    auto parts = Split(key, L':');
    return parts.size() < 4 ? 0 : static_cast<DWORD>(std::wcstoul(parts[3].c_str(), nullptr, 10));
}

}  // namespace xww::journal
