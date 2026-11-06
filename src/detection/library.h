//  Install folders of the games the launchers on this PC know about, read
//  from their own manifests and registry entries: Steam, Epic Games, GOG,
//  Ubisoft Connect, EA, Battle.net, Riot and Rockstar. A game installed to a
//  custom folder is still recognized as one. Rebuilt at most every few
//  minutes, when a detection asks for it.
#pragma once

#include "core/common.h"

#include <mutex>

namespace xww {

class GameLibrary {
public:
    std::wstring Store(const std::wstring& exePath);       // "Steam", "Epic Games"..., or empty

private:
    void Refresh();
    void Add(const std::wstring& folder, const wchar_t* store);

    std::mutex gate_;
    ULONGLONG built_ = 0;
    std::vector<std::pair<std::wstring, std::wstring>> folders_;   // lower-case folder with a trailing backslash, store
};

}  // namespace xww
