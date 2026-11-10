#pragma once
// Loopback HTTP/1.1 range server for tests (Winsock, one select() thread). After Start it serves from caller
// memory with fixed buffers and no allocation, so it can run inside an allocation-probed process. Behaviour
// is adjustable at run time through atomics: latency, bandwidth, range support, fault injection and object
// mutation (a new ETag, so If-Match requests fail with 412).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace loopback {
class Server {
public:
    std::atomic<unsigned> latency_ms{0};    // Delay before each response is sent.
    std::atomic<unsigned> bytes_per_ms{0};  // Per-connection body throttle; 0 is unlimited.
    std::atomic<bool> ranges{true};         // False: Range is ignored and every GET answers 200 with the full body.
    std::atomic<unsigned> drop_gets{0};     // The next N GETs close the connection after `drop_after` body bytes.
    std::atomic<unsigned> drop_after{0};
    std::atomic<unsigned> fail_gets{0};     // The next N GETs answer 503.
    std::atomic<bool> require_auth{false};  // API 15 tests: without "Authorization: Bearer loopback-token", 401.
    std::atomic<unsigned> unauthorized{0};  // 401s answered.
    std::atomic<unsigned> gets{0}, heads{0}, preconditions{0};
    std::atomic<std::uint64_t> body_bytes{0};

    ~Server() { Stop(); }
    bool Start(const std::byte* data, std::uint64_t size) {
        data_ = data; size_ = size;
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
        started_ = true;
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        if (listen_ == INVALID_SOCKET || bind(listen_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
            listen(listen_, SOMAXCONN) || getsockname(listen_, reinterpret_cast<sockaddr*>(&address), &length)) return false;
        port_ = ntohs(address.sin_port);
        u_long nonblocking = 1; ioctlsocket(listen_, FIONBIO, &nonblocking);
        char warm[64]; std::snprintf(warm, sizeof(warm), "%llu", 1ull); // Any first-use CRT setup happens here.
        thread_ = std::thread([this] { Loop(); });
        return true;
    }
    void Stop() {
        if (thread_.joinable()) { stop_.store(true); thread_.join(); }
        for (auto& c : connections_) Drop(c);
        if (listen_ != INVALID_SOCKET) { closesocket(listen_); listen_ = INVALID_SOCKET; }
        if (started_) { WSACleanup(); started_ = false; }
    }
    unsigned short Port() const { return port_; }
    void Mutate() { mutated_.store(true); } // The object "changes": a new strong ETag.
    const char* ETag() const { return mutated_.load() ? "\"fixture-v2\"" : "\"fixture-v1\""; }

private:
    struct Connection {
        SOCKET socket = INVALID_SOCKET;
        char in[8192]{};
        char head[512]{};
        unsigned in_length = 0, head_length = 0, head_sent = 0;
        std::uint64_t body = 0, body_end = 0, cut = UINT64_MAX; // cut: body position where the connection drops.
        std::int64_t due = 0, sent_at = 0;
        bool sending = false, close_after = false;
    };
    static std::int64_t Now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static void Drop(Connection& c) { if (c.socket != INVALID_SOCKET) closesocket(c.socket); c = Connection{}; }
    // Case-insensitive header value lookup inside the request head; returns the start of the value.
    static const char* Header(const char* head, const char* end, const char* name) {
        const auto length = std::strlen(name);
        for (const char* line = head; line < end; ) {
            const char* next = static_cast<const char*>(std::memchr(line, '\n', static_cast<std::size_t>(end - line)));
            if (!next) break;
            if (static_cast<std::size_t>(next - line) > length && _strnicmp(line, name, length) == 0 && line[length] == ':') {
                const char* value = line + length + 1;
                while (*value == ' ') ++value;
                return value;
            }
            line = next + 1;
        }
        return nullptr;
    }
    void Respond(Connection& c, const char* request, const char* end) {
        const bool head = std::strncmp(request, "HEAD ", 5) == 0;
        const bool get = std::strncmp(request, "GET ", 4) == 0;
        const char* target = request + (head ? 5 : 4);
        const bool missing = std::strncmp(target, "/missing", 8) == 0;
        c.close_after = Header(request, end, "Connection") && _strnicmp(Header(request, end, "Connection"), "close", 5) == 0;
        c.body = c.body_end = 0; c.cut = UINT64_MAX; c.head_sent = 0;
        int n = 0;
        if (get) ++gets; else if (head) ++heads;
        unsigned remaining = fail_gets.load();
        if (!get && !head) n = std::snprintf(c.head, sizeof(c.head), "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n");
        else if (missing) n = std::snprintf(c.head, sizeof(c.head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        else if (std::strncmp(target, "/redirect", 9) == 0)
            n = std::snprintf(c.head, sizeof(c.head), "HTTP/1.1 302 Found\r\nLocation: /fixture.bin\r\nContent-Length: 0\r\n\r\n");
        else if (require_auth.load() && !(Header(request, end, "Authorization") && std::strncmp(Header(request, end, "Authorization"), "Bearer loopback-token", 21) == 0)) {
            ++unauthorized;
            n = std::snprintf(c.head, sizeof(c.head), "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
        }
        else if (get && remaining && fail_gets.compare_exchange_strong(remaining, remaining - 1))
            n = std::snprintf(c.head, sizeof(c.head), "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
        else if (const char* match = Header(request, end, "If-Match"); match && std::strncmp(match, ETag(), std::strlen(ETag())) != 0 && *match != '*') {
            ++preconditions;
            n = std::snprintf(c.head, sizeof(c.head), "HTTP/1.1 412 Precondition Failed\r\nContent-Length: 0\r\n\r\n");
        } else {
            std::uint64_t first = 0, last = size_ ? size_ - 1 : 0;
            const char* range = ranges.load() ? Header(request, end, "Range") : nullptr;
            bool partial = false, bad = false;
            if (range && std::strncmp(range, "bytes=", 6) == 0) {
                char* next = nullptr;
                first = std::strtoull(range + 6, &next, 10);
                if (*next != '-') bad = true;
                else if (next[1] >= '0' && next[1] <= '9') last = (std::min)(std::strtoull(next + 1, nullptr, 10), last);
                bad = bad || first >= size_ || first > last;
                partial = true;
            }
            if (bad) n = std::snprintf(c.head, sizeof(c.head), "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%llu\r\nContent-Length: 0\r\n\r\n",
                                       static_cast<unsigned long long>(size_));
            else {
                const auto length = last - first + 1;
                if (partial) n = std::snprintf(c.head, sizeof(c.head), "HTTP/1.1 206 Partial Content\r\nContent-Length: %llu\r\nContent-Range: bytes %llu-%llu/%llu\r\n"
                                               "Accept-Ranges: bytes\r\nETag: %s\r\nContent-Type: video/mp4\r\n\r\n", static_cast<unsigned long long>(length),
                                               static_cast<unsigned long long>(first), static_cast<unsigned long long>(last), static_cast<unsigned long long>(size_), ETag());
                else n = std::snprintf(c.head, sizeof(c.head), "HTTP/1.1 200 OK\r\nContent-Length: %llu\r\nETag: %s\r\nContent-Type: video/mp4\r\n\r\n",
                                       static_cast<unsigned long long>(size_), ETag());
                if (get) {
                    c.body = first; c.body_end = first + length;
                    unsigned drops = drop_gets.load();
                    if (drops && drop_gets.compare_exchange_strong(drops, drops - 1)) c.cut = first + drop_after.load();
                }
            }
        }
        c.head_length = n > 0 ? static_cast<unsigned>(n) : 0;
        c.sending = true; c.due = Now() + latency_ms.load(); c.sent_at = c.due;
    }
    void Send(Connection& c, std::int64_t now) {
        if (c.head_sent < c.head_length) {
            const int n = send(c.socket, c.head + c.head_sent, static_cast<int>(c.head_length - c.head_sent), 0);
            if (n == SOCKET_ERROR) { if (WSAGetLastError() != WSAEWOULDBLOCK) Drop(c); return; }
            c.head_sent += static_cast<unsigned>(n);
            if (c.head_sent < c.head_length) return;
        }
        while (c.body < c.body_end) {
            std::uint64_t allow = c.body_end - c.body;
            if (const auto rate = bytes_per_ms.load()) {
                const auto budget = static_cast<std::uint64_t>(now - c.sent_at) * rate;
                if (!budget) return;
                allow = (std::min)(allow, budget);
            }
            if (c.cut != UINT64_MAX) {
                if (c.body >= c.cut) { Drop(c); return; } // Injected transport failure mid-body.
                allow = (std::min)(allow, c.cut - c.body);
            }
            allow = (std::min)(allow, std::uint64_t{64 * 1024});
            const int n = send(c.socket, reinterpret_cast<const char*>(data_ + c.body), static_cast<int>(allow), 0);
            if (n == SOCKET_ERROR) { if (WSAGetLastError() != WSAEWOULDBLOCK) Drop(c); return; }
            c.body += static_cast<std::uint64_t>(n); body_bytes += static_cast<std::uint64_t>(n);
            if (bytes_per_ms.load()) { c.sent_at = now; if (c.body < c.body_end) return; }
        }
        if (c.cut != UINT64_MAX && c.body >= c.cut) { Drop(c); return; }
        c.sending = false;
        if (c.close_after) Drop(c);
    }
    void Loop() {
        while (!stop_.load()) {
            fd_set readable, writable; FD_ZERO(&readable); FD_ZERO(&writable);
            FD_SET(listen_, &readable);
            const auto now = Now();
            for (auto& c : connections_) {
                if (c.socket == INVALID_SOCKET) continue;
                if (!c.sending) FD_SET(c.socket, &readable);
                else if (now >= c.due) FD_SET(c.socket, &writable);
            }
            timeval wait{0, 1000};
            if (select(0, &readable, &writable, nullptr, &wait) == SOCKET_ERROR) { Sleep(1); continue; }
            if (FD_ISSET(listen_, &readable)) {
                const SOCKET accepted = accept(listen_, nullptr, nullptr);
                if (accepted != INVALID_SOCKET) {
                    Connection* slot = nullptr;
                    for (auto& c : connections_) if (c.socket == INVALID_SOCKET) { slot = &c; break; }
                    if (!slot) closesocket(accepted);
                    else {
                        u_long nonblocking = 1; ioctlsocket(accepted, FIONBIO, &nonblocking);
                        BOOL nodelay = TRUE; setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
                        slot->socket = accepted;
                    }
                }
            }
            for (auto& c : connections_) {
                if (c.socket == INVALID_SOCKET) continue;
                if (!c.sending && FD_ISSET(c.socket, &readable)) {
                    const int n = recv(c.socket, c.in + c.in_length, static_cast<int>(sizeof(c.in) - 1 - c.in_length), 0);
                    if (n <= 0) { Drop(c); continue; }
                    c.in_length += static_cast<unsigned>(n); c.in[c.in_length] = 0;
                }
                if (!c.sending) {
                    // One request at a time per connection (no pipelining): parse once its head is complete.
                    const char* end = std::strstr(c.in, "\r\n\r\n");
                    if (end) {
                        Respond(c, c.in, end + 2);
                        const auto used = static_cast<unsigned>(end + 4 - c.in);
                        std::memmove(c.in, c.in + used, c.in_length - used); c.in_length -= used; c.in[c.in_length] = 0;
                    } else if (c.in_length >= sizeof(c.in) - 1) { Drop(c); continue; }
                }
                if (c.sending && Now() >= c.due) Send(c, Now());
            }
        }
    }
    const std::byte* data_ = nullptr;
    std::uint64_t size_ = 0;
    SOCKET listen_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    bool started_ = false;
    std::atomic<bool> stop_{false}, mutated_{false};
    std::thread thread_;
    Connection connections_[32]{};
};
} // namespace loopback
