//  Overview, Optimizations, Games, Background, Logs and Settings. Every
//  setting goes to the service ("set"), which validates it, writes
//  config.toml and answers with the new config; the switch moves at once.
#include "config/config.h"
#include "config/toml.h"
#include "ui/icons.h"
#include "ui/window.h"

#include <shellapi.h>
#include <shobjidl.h>

namespace xww::ui {

using namespace metric;

namespace {

constexpr wchar_t kRepository[] = L"https://github.com/xwwraw/xwwpulse";

// Each state has its own icon and word; nothing relies on color.
std::pair<const char*, const wchar_t*> StepLook(StepState s) {
    switch (s) {
    case StepState::On: return { icons::Check, L"On" };
    case StepState::Paused: return { icons::Pause, L"Paused" };
    case StepState::Skipped: return { icons::Skip, L"Skipped" };
    case StepState::Failed: return { icons::Alert, L"Failed" };
    default: return { icons::Off, L"Off" };
    }
}

std::vector<std::wstring> TomlList(const std::string& text) {
    if (text.empty()) return {};
    return toml::Parse(L"x = " + Widen(text)).List(L"x", {});
}

void Open(const std::wstring& target, const wchar_t* args = nullptr) {
    ShellExecuteW(nullptr, L"open", target.c_str(), args, nullptr, SW_SHOWNORMAL);
}

}  // namespace

// ------------------------------------------------------------------ overview

// The frame rate as a big Geist Mono number with a caption, and beside it what game mode is doing, in words.
void Window::Hero(Flow& f) {
    bool game = model_.pid != 0;
    bool off = model_.phase == L"off" || model_.phase == L"disabled";
    bool boosting = game && model_.fps > 0 && model_.phase == L"game";
    D2D1_RECT_F card = Rect(f.x, f.y, f.width, 200);
    canvas_.Fill(card, color::Canvas, RadiusMd);
    canvas_.Stroke(card, color::Hairline, RadiusMd);

    float x = card.left + Space24, caption = Spec(Type::Caption).line, display = Spec(Type::DisplayXl).line;
    canvas_.Text(L"Frame rate", Rect(x, card.top + Space24, 300, caption), color::TextMuted, Style(Type::Caption));
    std::wstring big = game && model_.fps > 0 ? std::format(L"{:.0f}", model_.fps) : L"--";
    canvas_.Text(big, Rect(x - 4, card.top + Space24 + caption + Space16, Width(card) / 2, display), color::Text, Style(Type::DisplayXl));
    canvas_.Text(game ? model_.game + L" · presents per second" : L"No game in front", Rect(x, card.bottom - Space24 - caption, Width(card) / 2 - Space24, caption), color::TextMuted, Style(Type::Caption));

    const char* icon = icons::Check;
    std::wstring state = L"Watching for games", detail = L"A game is picked up within seconds of its first steady frames.";
    if (!model_.connected) { icon = icons::Alert; state = L"Service not running"; detail = L"Install the service to detect and optimize games."; }
    else if (off) { icon = icons::Pause; state = L"Game mode is off"; detail = L"Nothing is changed until you turn it on."; }
    else if (boosting) { icon = icons::Zap; state = L"Boost on"; detail = L"Power, CPU, GPU and background apps are tuned for " + model_.game + L"."; }
    else if (model_.phase == L"paused") { icon = icons::Pause; state = L"Paused"; detail = L"The game is in the background; power and background apps are back to normal."; }
    else if (game) { icon = icons::Refresh; state = L"Waiting for frames"; detail = L"Game mode applies once the game presents frames."; }
    else if (model_.phase == L"confirming") { icon = icons::Refresh; state = L"Checking the frame rate"; detail = L"A possible game is in front."; }

    float rx = card.left + Width(card) / 2 + Space24, rw = card.right - Space24 - rx, line = Spec(Type::Heading).line;
    auto detailStyle = Wrapped(Type::BodySm);
    float detailHeight = canvas_.Measure(detail, rw, detailStyle).height;
    float top = card.top + (Height(card) - line - Space8 - detailHeight) / 2;
    canvas_.Fill(Rect(card.left + Width(card) / 2, card.top + Space24, 1, Height(card) - Space48), color::Hairline);
    canvas_.Icon(icon, { rx, top + (line - IconMd) / 2 }, IconMd, color::Text);
    canvas_.Text(state, Rect(rx + IconMd + Space12, top, rw - IconMd - Space12, line), color::Text, Style(Type::Heading));
    canvas_.Text(detail, Rect(rx, top + line + Space8, rw, detailHeight + 2), color::TextMuted, detailStyle);
    f.y = card.bottom + Space32;
}

void Window::Overview(Flow& f) {
    bool game = model_.pid != 0;
    bool off = model_.phase == L"off" || model_.phase == L"disabled";
    std::vector<ButtonSpec> actions;
    if (model_.connected) {
        if (off) actions.push_back({ L"Turn on", icons::Play, ButtonStyle::Primary, [this] { agent_->Send(ipc::Message("resume")); Toast(L"Game mode is on."); } });
        else actions.push_back({ L"Pause", icons::Pause, ButtonStyle::Outline, [this] { agent_->Send(ipc::Message("pause")); Toast(L"Game mode is paused until you turn it on."); } });
    }
    ui_.Header(f, L"Overview", L"", actions);
    Hero(f);

    if (!model_.connected) {
        ui_.Group(f, L"Service", { RowSpec{ L"xwwPulse service", L"Detection and every system change run in a Windows service. Installing it needs administrator rights.", { { L"Not running" } },
            Control::Button({ L"Install service", icons::Power, ButtonStyle::Primary, [this] { RunService(L"--install"); } }) } });
    }

    if (game) {
        std::vector<ButtonSpec> buttons;
        if (!model_.forced) buttons.push_back({ L"Always optimize", nullptr, ButtonStyle::Outline, [this] { agent_->AddToList(L"ForcedGames", model_.exe); Toast(model_.game + L" is always optimized now."); } });
        buttons.push_back({ L"Never optimize", nullptr, ButtonStyle::Outline, [this] {
            Confirm(L"Never optimize " + model_.game + L"?", L"Game mode ends for it now and it is ignored from now on. You can undo this on the Games page.", L"Never optimize", [this] {
                agent_->AddToList(L"BlockedApps", model_.exe);
                Toast(model_.game + L" is ignored from now on.");
            });
        } });
        ui_.Group(f, L"Current game", {
            RowSpec{ model_.game, model_.exe, {}, Control::Buttons(buttons), true },
            RowSpec{ L"Why it counts as a game", model_.reasons },
            RowSpec{ L"Anti-cheat", model_.antiCheat.empty() ? L"None found. Every optimization can apply." : L"Only process-level changes apply; game threads are left alone.", {},
                Control::Label(model_.antiCheat.empty() ? L"None" : model_.antiCheat) },
        });
    }

    std::vector<RowSpec> rows;
    for (auto& s : model_.steps) {
        auto [icon, word] = StepLook(s.state);
        RowSpec row{ s.title, s.detail, {}, Control::Label(word) };
        row.icon = icon;
        rows.push_back(row);
    }
    if (!rows.empty())
        ui_.Group(f, game ? L"This session" : L"When a game starts", rows,
            game ? L"Everything here is undone when the game closes, when you switch away (power and background apps), or if xwwPulse stops." : L"");
}

// ------------------------------------------------------------------ optimizations

void Window::Optimizations(Flow& f) {
    auto set = [this](const std::string& key, const std::string& value) {
        if (!agent_ || !agent_->Connected()) { Toast(L"The xwwPulse service is not running. Install it in Settings.", Tone::Alert); return; }
        model_.config[key] = value;
        agent_->Set(key, value);
    };
    auto toggle = [&](const std::string& key) {
        return Control::Switch(Widen(key), model_.Flag(key), [set, key](bool on) { set(key, on ? "true" : "false"); }, model_.connected);
    };
    auto segments = [&](const std::string& key, std::vector<std::wstring> labels, std::vector<std::string> values, std::function<bool(const std::string&)> guard = nullptr) {
        auto current = model_.Setting(key);
        int index = -1;
        for (size_t i = 0; i < values.size(); ++i) if (values[i] == current) index = static_cast<int>(i);
        return Control::Segments(Widen(key), std::move(labels), index, [set, key, values, guard](int i) { if (!guard || guard(values[i])) set(key, values[i]); });
    };

    ui_.Header(f, L"Optimizations", L"What changes while a game is in front. Everything is undone when it closes.", {
        { L"Restore defaults", icons::Reset, ButtonStyle::Outline, [this, set] {
            Confirm(L"Restore the default optimizations?", L"Every setting on this page goes back to its default. The lists on the Games and Background pages stay as they are.", L"Restore defaults", [set] {
                for (auto& [k, v] : Config::Defaults().Flatten()) if (k.find("_list") == std::string::npos && k != "general.start_with_windows") set(k, v);
            }, false);
        } },
    });

    ui_.Group(f, L"General", {
        { L"Game mode", L"Detect games and optimize them automatically.", {}, toggle("general.enabled") },
        { L"Pause on Alt+Tab", L"Power and background changes are undone while the game is not in front, and come back when it is.", {}, toggle("general.pause_on_alt_tab") },
        { L"Notifications", L"Show a notification when game mode turns on.", {}, toggle("general.notifications") },
    });

    ui_.Group(f, L"Processor", {
        { L"Processor idle states", L"Keeps cores out of deep C-states while you play, in a temporary copy of your power plan. Cores run warmer.", {}, toggle("power.idle_disable") },
        { L"Skip on battery", L"Leave idle states alone when the PC runs on battery.", {}, toggle("power.skip_on_battery") },
        { L"CPU placement", L"Which cores the game prefers: the V-Cache CCD on dual-CCD Ryzen, the P-cores on Intel hybrid." + (model_.topology.empty() ? L"" : L" " + model_.topology + L"."), {},
            segments("cpu.ccd", { L"Auto", L"CCD0", L"V-Cache", L"Off" }, { "auto", "ccd0", "vcache", "off" }) },
        { L"Placement mode", L"Soft uses CPU Sets, which the scheduler may override under load. Hard sets the affinity mask.", {},
            segments("cpu.cpu_set_mode", { L"Soft", L"Hard" }, { "soft", "hard" }) },
        { L"Background apps on other cores", L"Discord, browsers, Spotify, OBS and the [cores] list in background.toml run on the cores the game does not use.", {}, toggle("cpu.background_cores") },
        { L"Game thread boost", L"The busiest game threads lose power throttling and get their own physical cores. Turns itself off when an anti-cheat is running.", { { L"Opt-in", true } }, toggle("cpu.thread_boost") },
        { L"Thread boost with anti-cheat", L"Opens game threads even when an anti-cheat runs. Some anti-cheats log this.", { { L"Risky", true } },
            Control::Switch(L"cpu.thread_boost_with_anticheat", model_.Flag("cpu.thread_boost_with_anticheat"), [this, set](bool on) {
                if (!on) { set("cpu.thread_boost_with_anticheat", "false"); return; }
                Confirm(L"Allow thread boost with anti-cheat?", L"xwwPulse never injects into a game or reads its memory, but opening game threads can still be logged by EAC, BattlEye or Vanguard. Use this only for games without kernel anti-cheat bans.", L"Allow", [set] { set("cpu.thread_boost_with_anticheat", "true"); });
            }, model_.connected) },
    }, L"Process-level changes use the same rights Task Manager uses to change priority. Nothing is loaded into the game.");

    ui_.Group(f, L"Graphics", {
        { L"CPU priority", L"Priority class of the game process.", {}, segments("gpu.process_priority", { L"Normal", L"Above", L"High" }, { "normal", "above_normal", "high" }) },
        { L"GPU priority", L"Scheduling priority of the game on the GPU. Realtime can freeze the desktop if the game hangs.", {},
            segments("gpu.priority", { L"Off", L"High", L"Realtime" }, { "off", "high", "realtime" }, [this, set](const std::string& v) {
                if (v != "realtime") return true;
                Confirm(L"Use realtime GPU priority?", L"A game that hangs on the GPU can then starve the desktop compositor: a black screen and a frozen cursor until Windows resets the driver. High is safe for nearly every game.", L"Use realtime", [set] { set("gpu.priority", "realtime"); });
                return false;
            }) },
        { L"DWM multimedia scheduling", L"Runs the desktop compositor's threads under MMCSS while you play.", {}, toggle("gpu.dwm_mmcss") },
    });

    ui_.Group(f, L"Detection", {
        { L"Minimum frame rate", L"Average frames per second a possible game must hold for two seconds before game mode turns on.", {},
            segments("detection.min_fps", { L"15", L"20", L"30", L"45" }, { "15", "20", "30", "45" }) },
    }, L"A game is recognized by your lists, the Windows game list, the libraries of Steam, Epic Games, GOG, Ubisoft Connect, EA and Battle.net, its window and engine files, and then its frame rate.");
}

// ------------------------------------------------------------------ games

std::wstring Window::BrowseExe(const wchar_t* title) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) return {};
    COMDLG_FILTERSPEC filter{ L"Programs", L"*.exe" };
    dialog->SetFileTypes(1, &filter);
    dialog->SetTitle(title);
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(hwnd_)) || FAILED(dialog->GetResult(item.GetAddressOf())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::wstring exe = path;
    CoTaskMemFree(path);
    return exe;
}

void Window::AddGame(const wchar_t* list) {
    if (!agent_) return;
    bool forced = std::wstring(list) == L"ForcedGames";
    Pick(forced ? L"Choose a game to always optimize" : L"Choose an app to never optimize", [this, list, forced](const std::wstring& exe) {
        agent_->AddToList(list, exe);
        Toast(FriendlyName(exe) + (forced ? L" is always optimized now." : L" is ignored from now on."));
    });
}

void Window::Games(Flow& f) {
    ui_.Header(f, L"Games", L"Games are found automatically. These lists override the detection and are stored for your account only.", {
        { L"Add game", icons::Plus, ButtonStyle::Primary, [this] { if (agent_) AddGame(L"ForcedGames"); } },
    });
    auto listRows = [&](const wchar_t* list, const wchar_t* empty, const wchar_t* emptyDetail) {
        std::vector<RowSpec> rows;
        for (auto& exe : Agent::List(list))
            rows.push_back({ FriendlyName(exe), exe, {}, Control::Button({ L"Remove", nullptr, ButtonStyle::Outline, [this, list, exe] { agent_->RemoveFromList(list, exe); Toast(FileName(exe) + L" is no longer on the list."); } }), true });
        if (rows.empty()) rows.push_back({ empty, emptyDetail });
        return rows;
    };
    ui_.Group(f, L"Always optimize", listRows(L"ForcedGames", L"No games added", L"Games you add are optimized as soon as they are in front, without waiting for the frame rate check."));
    auto blocked = listRows(L"BlockedApps", L"No apps added", L"Apps you add here are never treated as games.");
    blocked.insert(blocked.begin(), RowSpec{ L"Add an app", L"For tools that look like games to xwwPulse: emulator front ends, benchmarks, video tools.", {},
        Control::Button({ L"Choose app", nullptr, ButtonStyle::Outline, [this] { if (agent_) AddGame(L"BlockedApps"); } }) });
    ui_.Group(f, L"Never optimize", blocked);

    std::vector<RowSpec> recent;
    for (auto& d : model_.history) {
        ButtonSpec button = d.verdict != L"game"
            ? ButtonSpec{ L"Always optimize", nullptr, ButtonStyle::Outline, [this, d] { agent_->AddToList(L"ForcedGames", d.exe); Toast(d.name + L" is always optimized now."); } }
            : ButtonSpec{ L"Never optimize", nullptr, ButtonStyle::Outline, [this, d] { agent_->AddToList(L"BlockedApps", d.exe); Toast(d.name + L" is ignored from now on."); } };
        RowSpec row{ d.name, d.reasons.empty() ? FileName(d.exe) : d.reasons, { { d.verdict == L"game" ? L"Game" : d.verdict == L"undecided" ? L"Undecided" : L"Not a game" } }, Control::Button(button) };
        row.icon = d.verdict == L"game" ? icons::Check : d.verdict == L"undecided" ? icons::Refresh : icons::Ban;
        recent.push_back(row);
    }
    if (recent.empty()) recent.push_back({ L"No decisions yet", L"Apps you bring to the front show up here with the reason for the decision." });
    ui_.Group(f, L"Recent decisions", recent);
}

// ------------------------------------------------------------------ background

// The lists live in background.toml in ProgramData, which only the service may write; edits go to it like any setting.
void Window::EditList(const std::string& key, std::vector<std::wstring> names) {
    if (!agent_ || !agent_->Connected()) { Toast(L"The xwwPulse service is not running. Install it in Settings.", Tone::Alert); return; }
    std::wstring joined, toml = L"[";
    for (size_t i = 0; i < names.size(); ++i) {
        joined += (i ? L"|" : L"") + names[i];
        toml += (i ? L", " : L"") + toml::Quote(names[i]);
    }
    model_.config[key] = Narrow(toml + L"]");
    agent_->Set(key, Narrow(joined));
}

void Window::Background(Flow& f) {
    auto set = [this](const std::string& key, bool on) {
        if (!agent_ || !agent_->Connected()) { Toast(L"The xwwPulse service is not running. Install it in Settings.", Tone::Alert); return; }
        model_.config[key] = on ? "true" : "false";
        agent_->Set(key, on ? "true" : "false");
    };
    auto defaults = Config::Defaults();
    ui_.Header(f, L"Background", L"Apps limited while a game is in front. An app you switch to is released at once.", {
        { L"Restore defaults", icons::Reset, ButtonStyle::Outline, [this, set, defaults] {
            Confirm(L"Restore the default lists?", L"The three lists go back to the apps xwwPulse ships with, and all three switches turn on. Apps you added are taken off.", L"Restore defaults", [this, set, defaults] {
                EditList("background.suspend_list", defaults.suspendList);
                EditList("background.throttle_list", defaults.throttleList);
                EditList("background.cores_list", defaults.coreList);
                for (auto key : { "background.suspend", "background.throttle", "cpu.background_cores" }) set(key, true);
                Toast(L"The background lists are back to their defaults.");
            }, false);
        } },
    });
    auto group = [&](const wchar_t* title, const std::string& key, const std::string& listKey, const std::vector<std::wstring>& builtIn, const wchar_t* what, const wchar_t* how, const wchar_t* pickTitle) {
        auto names = TomlList(model_.Setting(listKey));
        std::vector<RowSpec> rows{ { what, how, {}, Control::Switch(Widen(key), model_.Flag(key), [set, key](bool on) { set(key, on); }, model_.connected) } };
        // default entries you removed can come back one list at a time
        std::vector<std::wstring> missing;
        for (auto& name : builtIn) if (std::find(names.begin(), names.end(), name) == names.end()) missing.push_back(name);
        if (!missing.empty()) {
            std::wstring list;
            for (auto& name : missing) list += (list.empty() ? L"" : L", ") + name;
            rows.push_back({ std::format(L"{} default {} removed", missing.size(), missing.size() == 1 ? L"app" : L"apps"), list, {},
                Control::Button({ L"Restore", icons::Reset, ButtonStyle::Outline, [this, listKey, names, missing] {
                    auto all = names;
                    all.insert(all.end(), missing.begin(), missing.end());
                    EditList(listKey, all);
                    Toast(L"The default apps are back on the list.");
                } }) });
        }
        for (auto& name : names)
            rows.push_back({ name, L"", std::find(builtIn.begin(), builtIn.end(), name) != builtIn.end() ? std::vector<BadgeSpec>{ { L"Default", true } } : std::vector<BadgeSpec>{},
                Control::Button({ L"Remove", nullptr, ButtonStyle::Outline, [this, listKey, names, name] {
                auto rest = names;
                std::erase(rest, name);
                EditList(listKey, rest);
                Toast(name + L" is no longer on the list.");
            } }) });
        rows.push_back({ L"Add an app", L"Choose a running app or an exe file.", {}, Control::Button({ L"Add app", icons::Plus, ButtonStyle::Outline, [this, listKey, names, pickTitle] {
            Pick(pickTitle, [this, listKey, names](const std::wstring& exe) {
                auto name = FileName(exe);
                if (std::find(names.begin(), names.end(), name) != names.end()) { Toast(name + L" is already on the list.", Tone::Neutral); return; }
                auto more = names;
                more.push_back(name);
                EditList(listKey, more);
                Toast(name + L" is on the list now.");
            });
        } }) });
        ui_.Group(f, title, rows);
    };
    group(L"Suspend", "background.suspend", "background.suspend_list", defaults.suspendList, L"Suspend these apps", L"Frozen completely with NtSuspendProcess and resumed when the game closes. Do not add apps you need while you play, such as voice chat.", L"Choose an app to suspend");
    group(L"Throttle", "background.throttle", "background.throttle_list", defaults.throttleList, L"Throttle these apps", L"Efficiency mode (EcoQoS), very low I/O priority and no dynamic priority boost.", L"Choose an app to throttle");
    group(L"Other cores", "cpu.background_cores", "background.cores_list", defaults.coreList, L"Move these apps to other cores", L"They run on the cores the game does not use: the CCD without V-Cache, or the E-cores.", L"Choose an app for the other cores");
    ui_.Group(f, L"Never touched", { { L"The game, its launcher and xwwPulse", L"The game's parent processes (Steam, Epic Games, Battle.net) keep running normally, and so does anything you bring to the front during the session." } },
        L"The lists are saved to " + Config::BackgroundPath() + L" and apply from the next game session.");
}

// ------------------------------------------------------------------ logs

void Window::Logs(Flow& f) {
    auto file = LogDir() + L"\\Pulse.log";
    if (logLines_.empty()) {
        Handle h(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
        if (h) {
            LARGE_INTEGER size{};
            GetFileSizeEx(h.get(), &size);
            LONGLONG start = std::max<LONGLONG>(0, size.QuadPart - 48 * 1024);
            LARGE_INTEGER at{ .QuadPart = start };
            SetFilePointerEx(h.get(), at, nullptr, FILE_BEGIN);
            std::string data(static_cast<size_t>(size.QuadPart - start), '\0');
            DWORD read = 0;
            ReadFile(h.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
            data.resize(read);
            auto lines = Split(Widen(data), L'\n');
            if (start > 0 && !lines.empty()) lines.erase(lines.begin());
            for (auto& l : lines) if (auto t = Trim(l); !t.empty()) logLines_.push_back(t);
            if (logLines_.size() > 150) logLines_.erase(logLines_.begin(), logLines_.end() - 150);
        }
        if (logLines_.empty()) logLines_.push_back(L"Nothing is logged yet. Detections and every change the service makes appear here.");
    }
    ui_.Header(f, L"Logs", L"What the service did: detections, every change it made and every rollback.", {
        { L"Refresh", icons::Refresh, ButtonStyle::Outline, [this] { logLines_.clear(); Invalidate(); } },
        { L"Open folder", icons::Folder, ButtonStyle::Outline, [] { Open(LogDir()); } },
        { L"Copy", icons::Copy, ButtonStyle::Outline, [this] {
            std::wstring all;
            for (auto& l : logLines_) all += l + L"\r\n";
            if (OpenClipboard(hwnd_)) {
                EmptyClipboard();
                if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (all.size() + 1) * sizeof(wchar_t))) {
                    memcpy(GlobalLock(mem), all.c_str(), (all.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(mem);
                    SetClipboardData(CF_UNICODETEXT, mem);
                }
                CloseClipboard();
                Toast(L"The log is copied to the clipboard.");
            }
        } },
    });
    // a code block: recess, a wire edge, radius 4; a failed step leads with an alert icon
    float line = Spec(Type::Code).line;
    D2D1_RECT_F block = Rect(f.x, f.y, f.width, logLines_.size() * line + Space16 * 2);
    canvas_.Fill(block, color::Recess, RadiusSm);
    canvas_.Stroke(block, color::Wire, RadiusSm);
    float y = block.top + Space16;
    for (auto& l : logLines_) {
        bool error = Contains(l, L"fail") || Contains(l, L"crash") || Contains(l, L"error");
        if (error) canvas_.Icon(icons::Alert, { block.left + Space16, y + (line - IconSm) / 2 }, IconSm, color::Text);
        float x = block.left + Space16 + IconSm + Space8;
        canvas_.Text(l, Rect(x, y, block.right - Space16 - x, line), error ? color::Text : color::TextSoft, Style(Type::Code));
        y += line;
    }
    f.y = block.bottom + Space8;
    canvas_.Text(file, Rect(f.x, f.y, f.width, line), color::TextMuted, Style(Type::Code));
    f.y += line + Space32;
}

// ------------------------------------------------------------------ settings

void Window::RunService(const wchar_t* arguments) {
    if (!agent_) return;
    auto exe = ModuleDir() + L"\\xwwPulseService.exe";
    SHELLEXECUTEINFOW info{ sizeof(info) };
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = hwnd_;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = arguments;
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) { Toast(L"The service was not changed: Windows did not allow it to start as administrator.", Tone::Alert); return; }
    if (info.hProcess) { WaitForSingleObject(info.hProcess, 15000); CloseHandle(info.hProcess); }
    Toast(std::wstring(arguments) == L"--install" ? L"The xwwPulse service is installed." : L"The xwwPulse service is removed.");
}

// The installed copy keeps uninstall.exe next to the app; it asks for administrator rights and confirmation itself.
void Window::Uninstall() {
    auto uninstaller = ModuleDir() + L"\\uninstall.exe";
    if (GetFileAttributesW(uninstaller.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Confirm(L"Uninstall the xwwPulse service?", L"This copy of xwwPulse was not installed with the setup, so only the service can be removed. Game mode stops and every change still active is undone.", L"Uninstall service", [this] { RunService(L"--uninstall"); });
        return;
    }
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", uninstaller.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        Toast(L"The uninstaller did not start. Use Settings > Apps > Installed apps in Windows instead.", Tone::Alert);
}

void Window::SettingsPage(Flow& f) {
    ui_.Header(f, L"Settings", L"Preferences for xwwPulse itself.");
    ui_.Group(f, L"Service", {
        { L"xwwPulse service", model_.connected ? L"Running as SYSTEM. Detection and every system change happen there." : L"Not running. Installing it needs administrator rights.",
            { model_.connected ? BadgeSpec{ L"Running" } : BadgeSpec{ L"Stopped" } },
            Control::Button({ model_.connected ? L"Reinstall" : L"Install service", icons::Power, model_.connected ? ButtonStyle::Outline : ButtonStyle::Primary, [this] { RunService(L"--install"); } }) },
        { L"Start at sign-in", L"xwwPulse starts in the notification area when you sign in. Games are detected only while it runs.", {},
            Control::Switch(L"general.start_with_windows", model_.Flag("general.start_with_windows"), [this](bool on) {
                if (!agent_ || !agent_->Connected()) { Toast(L"The xwwPulse service is not running. Install it in Settings.", Tone::Alert); return; }
                model_.config["general.start_with_windows"] = on ? "true" : "false";
                agent_->Set("general.start_with_windows", on ? "true" : "false");
            }, model_.connected) },
        { L"Settings folder", Config::ConfigPath(), {}, Control::Button({ L"Open folder", icons::Folder, ButtonStyle::Outline, [] { Open(ProgramDataDir()); } }), true },
    });

    ui_.Group(f, L"Remove", {
        { L"Uninstall xwwPulse", L"Removes the service, the app, its shortcut and its Apps entry. Every change still active is undone first; you choose whether your settings and logs go too.", {},
            Control::Button({ L"Uninstall xwwPulse", nullptr, ButtonStyle::Outline, [this] { Uninstall(); } }) },
    });

    About(f);
}

// A section marker, the version and the links, and the sign-off line.
void Window::About(Flow& f) {
    f.y += Space32;
    float display = Spec(Type::Display).line;
    float slashes = canvas_.Measure(L"//", 400, Style(Type::Display)).width;
    canvas_.Text(L"//", Rect(f.x - 4, f.y, slashes + 4, display), color::TextMuted, Style(Type::Display));
    canvas_.Text(L"about", Rect(f.x - 4 + slashes, f.y, 400, display), color::Text, Style(Type::Display));
    float icon = 56;
    canvas_.AppIcon(Rect(f.x + f.width - icon, f.y + (display - icon) / 2, icon, icon));
    f.y += display + Space32;

    ui_.Group(f, L"", {
        { L"Version", L"", {}, Control::Label(model_.version) },
        { L"Release notes", L"What changed in each version.", {}, Control::Button({ L"Open", icons::Link, ButtonStyle::Text, [] { Open(std::wstring(kRepository) + L"/releases"); } }) },
        { L"Source code", L"github.com/xwwraw/xwwpulse", {}, Control::Button({ L"Open", icons::Link, ButtonStyle::Text, [] { Open(kRepository); } }) },
        { L"License", L"MIT. Geist and Geist Mono are licensed under the SIL Open Font License 1.1, Lucide icons under ISC.", {}, Control::Button({ L"Open", icons::Link, ButtonStyle::Text, [] { Open(std::wstring(kRepository) + L"/blob/main/LICENSE"); } }) },
    });

    // the sign-off: the mark in a font that has kana, the words in Geist
    std::wstring words = L" made by xwwraw";
    float caption = Spec(Type::Caption).line;
    float markWidth = canvas_.Measure(L"ゝ", 100, Style(Type::Mark)).width, wordsWidth = canvas_.Measure(words, 400, Style(Type::Caption)).width;
    float x = f.x + (f.width - markWidth - wordsWidth) / 2;
    canvas_.Text(L"ゝ", Rect(x, f.y, markWidth + 1, caption), color::TextMuted, Style(Type::Mark));
    canvas_.Text(words, Rect(x + markWidth, f.y, wordsWidth + 1, caption), color::TextMuted, Style(Type::Caption));
    f.y += caption + Space16;
}

}  // namespace xww::ui
