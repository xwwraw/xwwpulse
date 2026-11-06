//  Service <-> agent messages over \\.\pipe\xwwPulse (message mode).
//  A message is one UTF-8 line: "type<TAB>key=value<TAB>key=value", with
//  %, TAB and newlines percent-encoded. The pipe's DACL admits interactive
//  users, SYSTEM and administrators only.
//
//  agent -> service   hello      session, user
//                     foreground pid, hwnd facts (see detection/facts.h), list verdict
//                     set        key, value          (a config.toml setting)
//                     pause / resume / rescan
//  service -> agent   status     state, game, pid, fps, optimizations, anticheat
//                     mmcss      on=0|1              (DwmEnableMMCSS runs in the agent)
//                     config     the whole config as key=value pairs
//                     detected   exe, verdict, score (history for the Games page)
#pragma once

#include "core/common.h"

#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>

namespace xww::ipc {

struct Message {
    std::string type;
    std::map<std::string, std::string> fields;

    Message() = default;
    explicit Message(std::string t) : type(std::move(t)) {}
    Message& Set(const std::string& key, const std::string& value) { fields[key] = value; return *this; }
    Message& Set(const std::string& key, const std::wstring& value) { fields[key] = Narrow(value); return *this; }
    Message& Set(const std::string& key, long long value) { fields[key] = std::to_string(value); return *this; }
    Message& Set(const std::string& key, double value) { fields[key] = std::format("{:.1f}", value); return *this; }
    Message& Set(const std::string& key, bool value) { fields[key] = value ? "1" : "0"; return *this; }

    std::string Get(const std::string& key, const std::string& fallback = {}) const;
    std::wstring GetW(const std::string& key) const { return Widen(Get(key)); }
    long long GetInt(const std::string& key, long long fallback = 0) const;
    double GetDouble(const std::string& key, double fallback = 0) const;
    bool GetBool(const std::string& key) const { return Get(key) == "1"; }

    std::string Encode() const;
    static std::optional<Message> Decode(std::string_view line);
};

// ------------------------------------------------------------------ server (service)

class Server {
public:
    using ClientId = uint32_t;
    std::function<void(ClientId, const Message&)> OnMessage;
    std::function<void(ClientId)> OnConnect, OnDisconnect;

    bool Start();
    void Stop();
    bool Send(ClientId client, const Message& message);
    void Broadcast(const Message& message);
    ~Server() { Stop(); }

private:
    struct Client {
        ClientId id;
        Handle pipe;
        std::mutex writeGate;
        std::thread reader;
    };
    void AcceptLoop();
    void ReadLoop(std::shared_ptr<Client> client);

    std::atomic<bool> running_{ false };
    Handle stop_;
    std::thread acceptor_;
    std::mutex gate_;
    std::map<ClientId, std::shared_ptr<Client>> clients_;
    ClientId next_ = 1;
};

// ------------------------------------------------------------------ client (agent)

class Client {
public:
    std::function<void(const Message&)> OnMessage;
    std::function<void(bool connected)> OnConnection;

    void Start();               // connects in the background and reconnects when the service restarts
    void Stop();
    bool Send(const Message& message);
    bool Connected() const { return connected_; }
    ~Client() { Stop(); }

private:
    void Run();
    std::atomic<bool> running_{ false }, connected_{ false };
    Handle stop_;
    std::thread worker_;
    std::mutex gate_;
    Handle pipe_;
};

}  // namespace xww::ipc
