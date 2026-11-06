//  What the dashboard shows, filled from service messages on the window
//  thread: session status, the optimization steps, settings, the recent
//  detection decisions and a minute of frame rate history.
#pragma once

#include "core/ipc.h"
#include "optimizer/steps.h"

#include <deque>
#include <map>

namespace xww::ui {

struct Detection { std::wstring exe, name, verdict, reasons; };

struct Model {
    bool connected = false;
    std::wstring phase = L"idle";               // idle | confirming | game | paused | off | disabled
    std::wstring game, exe, reasons, antiCheat, version = XWW_VERSION, topology;
    DWORD pid = 0;
    double fps = 0;
    bool forced = false;
    std::vector<Step> steps;
    std::map<std::string, std::string> config;
    std::deque<Detection> history;
    std::deque<float> fpsSeries;

    std::string Setting(const std::string& key, const std::string& fallback = {}) const {
        auto it = config.find(key);
        return it == config.end() ? fallback : it->second;
    }
    bool Flag(const std::string& key) const { return Setting(key) == "true"; }

    void Apply(const ipc::Message& m) {
        if (m.type == "status") {
            phase = m.GetW("phase");
            game = m.GetW("game");
            exe = m.GetW("exe");
            reasons = m.GetW("reasons");
            antiCheat = m.GetW("anticheat");
            forced = m.GetBool("forced");
            pid = static_cast<DWORD>(m.GetInt("pid"));
            fps = m.GetDouble("fps");
            if (!m.Get("version").empty()) version = m.GetW("version");
            steps = DecodeSteps(m.GetW("steps"));
            if (pid) { fpsSeries.push_back(static_cast<float>(fps)); if (fpsSeries.size() > 60) fpsSeries.pop_front(); }
            else fpsSeries.clear();
        } else if (m.type == "config") {
            config.clear();
            for (auto& [k, v] : m.fields) if (k != "topology") config[k] = v;
            topology = m.GetW("topology");
        } else if (m.type == "detected") {
            Detection d{ m.GetW("exe"), m.GetW("name"), m.GetW("verdict"), m.GetW("reasons") };
            std::erase_if(history, [&](const Detection& x) { return x.exe == d.exe; });
            history.push_front(d);
            if (history.size() > 12) history.pop_back();
        }
    }
};

}  // namespace xww::ui
