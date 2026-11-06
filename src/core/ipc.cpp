//  Overlapped named-pipe server and client. Every wait also watches a stop
//  event, so Stop() never hangs on a blocked read.
#include "core/ipc.h"

#include <sddl.h>

namespace xww::ipc {

static constexpr DWORD kBuffer = 64 * 1024;

// ------------------------------------------------------------------ encoding

static std::string Escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '%' || c == '\t' || c == '\n' || c == '\r' || c == '=') out += std::format("%{:02X}", static_cast<unsigned char>(c));
        else out += c;
    }
    return out;
}

static std::string Unescape(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = std::string(s.substr(i + 1, 2));
            out += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}

std::string Message::Get(const std::string& key, const std::string& fallback) const {
    auto it = fields.find(key);
    return it == fields.end() ? fallback : it->second;
}

long long Message::GetInt(const std::string& key, long long fallback) const {
    auto value = Get(key);
    if (value.empty()) return fallback;
    try { return std::stoll(value); } catch (...) { return fallback; }
}

double Message::GetDouble(const std::string& key, double fallback) const {
    auto value = Get(key);
    if (value.empty()) return fallback;
    try { return std::stod(value); } catch (...) { return fallback; }
}

std::string Message::Encode() const {
    std::string line = Escape(type);
    for (auto& [k, v] : fields) line += "\t" + Escape(k) + "=" + Escape(v);
    return line;
}

std::optional<Message> Message::Decode(std::string_view line) {
    if (line.empty()) return std::nullopt;
    Message m;
    size_t start = 0;
    bool first = true;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i < line.size() && line[i] != '\t') continue;
        auto part = line.substr(start, i - start);
        start = i + 1;
        if (first) { m.type = Unescape(part); first = false; continue; }
        auto eq = part.find('=');
        if (eq == std::string_view::npos) continue;
        m.fields[Unescape(part.substr(0, eq))] = Unescape(part.substr(eq + 1));
    }
    return m.type.empty() ? std::nullopt : std::optional<Message>(std::move(m));
}

// ------------------------------------------------------------------ overlapped helpers

static bool WaitIo(HANDLE pipe, OVERLAPPED& ov, HANDLE stop, DWORD& bytes) {
    HANDLE events[2] = { ov.hEvent, stop };
    DWORD r = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, INFINITE);
    if (r != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &bytes, TRUE);
        SetLastError(ERROR_OPERATION_ABORTED);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &bytes, FALSE) != FALSE;
}

// Reads one whole message; false when the pipe broke or stop was signalled.
static bool ReadMessage(HANDLE pipe, HANDLE stop, std::string& out) {
    out.clear();
    std::string chunk(kBuffer, '\0');
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    for (;;) {
        OVERLAPPED ov{};
        ov.hEvent = event.get();
        ResetEvent(ov.hEvent);
        DWORD bytes = 0;
        BOOL ok = ReadFile(pipe, chunk.data(), kBuffer, &bytes, &ov);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error == ERROR_IO_PENDING) {
            ok = WaitIo(pipe, ov, stop, bytes);
            error = ok ? ERROR_SUCCESS : GetLastError();
        }
        if (ok) { out.append(chunk.data(), bytes); return true; }
        if (error == ERROR_MORE_DATA) { GetOverlappedResult(pipe, &ov, &bytes, FALSE); out.append(chunk.data(), bytes); continue; }
        return false;
    }
}

static bool WriteMessage(HANDLE pipe, const std::string& data) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED ov{};
    ov.hEvent = event.get();
    DWORD bytes = 0;
    if (WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &bytes, &ov)) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0) { CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &bytes, TRUE); return false; }
    return GetOverlappedResult(pipe, &ov, &bytes, FALSE) != FALSE;
}

// ------------------------------------------------------------------ server

bool Server::Start() {
    if (running_) return true;
    stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    running_ = true;
    acceptor_ = std::thread([this] { AcceptLoop(); });
    return true;
}

void Server::AcceptLoop() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    // protected DACL: SYSTEM and administrators full, interactive users read/write
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd, nullptr);
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
    bool first = true;
    while (running_) {
        Handle pipe(CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, kBuffer, kBuffer, 0, &sa));
        if (!pipe) {
            Log(L"IPC", L"CreateNamedPipe failed ({}); another instance may own the pipe", GetLastError());
            if (WaitForSingleObject(stop_.get(), 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        first = false;
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED ov{};
        ov.hEvent = event.get();
        DWORD bytes = 0;
        bool connected = ConnectNamedPipe(pipe.get(), &ov) != FALSE;
        if (!connected) {
            DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) connected = true;
            else if (error == ERROR_IO_PENDING) connected = WaitIo(pipe.get(), ov, stop_.get(), bytes);
        }
        if (!connected || !running_) continue;

        auto client = std::make_shared<Client>();
        client->pipe = std::move(pipe);
        {
            std::lock_guard lock(gate_);
            client->id = next_++;
            clients_[client->id] = client;
        }
        client->reader = std::thread([this, client] { ReadLoop(client); });
    }
    LocalFree(sd);
}

void Server::ReadLoop(std::shared_ptr<Client> client) {
    if (OnConnect) OnConnect(client->id);
    std::string data;
    while (running_ && ReadMessage(client->pipe.get(), stop_.get(), data))
        if (auto m = Message::Decode(data); m && OnMessage) OnMessage(client->id, *m);
    if (OnDisconnect && running_) OnDisconnect(client->id);
    std::lock_guard lock(gate_);
    if (auto it = clients_.find(client->id); it != clients_.end()) {
        it->second->reader.detach();
        clients_.erase(it);
    }
}

bool Server::Send(ClientId id, const Message& message) {
    std::shared_ptr<Client> client;
    {
        std::lock_guard lock(gate_);
        auto it = clients_.find(id);
        if (it == clients_.end()) return false;
        client = it->second;
    }
    std::lock_guard lock(client->writeGate);
    return WriteMessage(client->pipe.get(), message.Encode());
}

void Server::Broadcast(const Message& message) {
    std::vector<ClientId> ids;
    {
        std::lock_guard lock(gate_);
        for (auto& [id, c] : clients_) ids.push_back(id);
    }
    for (auto id : ids) Send(id, message);
}

void Server::Stop() {
    if (!running_.exchange(false)) return;
    SetEvent(stop_.get());
    if (acceptor_.joinable()) acceptor_.join();
    std::map<ClientId, std::shared_ptr<Client>> clients;
    {
        std::lock_guard lock(gate_);
        clients.swap(clients_);
    }
    for (auto& [id, c] : clients) {
        CancelIoEx(c->pipe.get(), nullptr);
        if (c->reader.joinable()) c->reader.join();
        DisconnectNamedPipe(c->pipe.get());
    }
}

// ------------------------------------------------------------------ client

void Client::Start() {
    if (running_) return;
    stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    running_ = true;
    worker_ = std::thread([this] { Run(); });
}

// The server must be xwwPulseService.exe (the service, or --console while testing); a squatting process gets no data.
static bool TrustedServer(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;
    return EndsWith(ImagePath(pid), L"\\xwwPulseService.exe");
}

void Client::Run() {
    while (running_) {
        Handle pipe(CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (!pipe) {
            if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(kPipeName, 1000);
            else if (WaitForSingleObject(stop_.get(), 2000) == WAIT_OBJECT_0) break;
            continue;
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr);
        if (!TrustedServer(pipe.get())) {
            Log(L"IPC", L"the pipe is not served by the xwwPulse service; ignoring it");
            if (WaitForSingleObject(stop_.get(), 5000) == WAIT_OBJECT_0) break;
            continue;
        }
        {
            std::lock_guard lock(gate_);
            pipe_.reset(pipe.release());
        }
        connected_ = true;
        if (OnConnection) OnConnection(true);
        std::string data;
        while (running_ && ReadMessage(pipe_.get(), stop_.get(), data))
            if (auto m = Message::Decode(data); m && OnMessage) OnMessage(*m);
        connected_ = false;
        {
            std::lock_guard lock(gate_);
            pipe_.reset();
        }
        if (running_ && OnConnection) OnConnection(false);
        if (WaitForSingleObject(stop_.get(), 1000) == WAIT_OBJECT_0) break;
    }
}

bool Client::Send(const Message& message) {
    std::lock_guard lock(gate_);
    return pipe_ && WriteMessage(pipe_.get(), message.Encode());
}

void Client::Stop() {
    if (!running_.exchange(false)) return;
    SetEvent(stop_.get());
    if (worker_.joinable()) worker_.join();
}

}  // namespace xww::ipc
