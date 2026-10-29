// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Modbus/TCP server on the network: one thread, non-blocking sockets
// and poll. The simulation runs in real time (or faster or slower); the
// requests are answered between two cycles, so a client sees the values as
// they are at the end of a cycle, and what it writes is there for the next.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

#include "modbus.hpp"
#include "sim.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
using pollfd_t = WSAPOLLFD;
using socklen_t = int;
constexpr int kSendFlags = 0;
constexpr socket_t kNoSocket = INVALID_SOCKET;
#define S7F_POLL WSAPoll
#define S7F_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using socket_t = int;
using pollfd_t = pollfd;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // a closed peer gives EPIPE, not SIGPIPE
#else
constexpr int kSendFlags = 0;
#endif
constexpr socket_t kNoSocket = -1;
#define S7F_POLL poll
#define S7F_CLOSE close
#endif

namespace s7forge {
namespace {

#ifdef _WIN32
struct NetInit {
    bool ok = false;
    NetInit() {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~NetInit() {
        if (ok) WSACleanup();
    }
};
bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
void nonBlocking(socket_t s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
#else
struct NetInit {
    bool ok = true;
};
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
void nonBlocking(socket_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

struct Client {
    socket_t s = kNoSocket;
    std::string peer;
    std::vector<uint8_t> in, out;
    std::chrono::steady_clock::time_point last;  // its last whole request (or its connection)
    std::chrono::steady_clock::time_point partSince;  // since when a frame is incomplete
};

constexpr int kIdleSeconds = 60;     // a client without a whole request for this long is dropped
constexpr int kPartialSeconds = 10;  // a frame not completed in this time: the client is dropped
constexpr double kMaxLagSeconds = 1.0;  // a simulation further behind real time drops the rest

}  // namespace

int serveModbus(Simulator& sim, ModbusServer& server, const ServeOptions& opt, const std::atomic<bool>* stop,
                std::atomic<int>* boundPort, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        if (boundPort) *boundPort = -1;
        return 2;
    };
    NetInit net;
    if (!net.ok) return fail("the network could not be started");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(opt.port));
    if (inet_pton(AF_INET, opt.listen.c_str(), &addr.sin_addr) != 1) return fail("not an IPv4 address: " + opt.listen);
    socket_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == kNoSocket) return fail("no socket");
    int yes = 1;
#ifdef _WIN32
    // Windows lets two programs share a port with SO_REUSEADDR: this one wants it alone
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof yes);
#else
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
#endif
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        S7F_CLOSE(ls);
        return fail("cannot listen on " + opt.listen + ":" + std::to_string(opt.port) +
                    (opt.port < 1024 ? " (a port below 1024 needs administrator rights)" : " (in use?)"));
    }
    if (listen(ls, 8) != 0) {
        S7F_CLOSE(ls);
        return fail("cannot listen");
    }
    nonBlocking(ls);
    sockaddr_in got{};
    socklen_t len = sizeof got;
    getsockname(ls, reinterpret_cast<sockaddr*>(&got), &len);
    const int port = ntohs(got.sin_port);
    if (boundPort) *boundPort = port;
    if (opt.verbose) std::cerr << "s7forge: Modbus/TCP on " << opt.listen << ":" << port << "\n";

    std::vector<std::unique_ptr<Client>> clients;
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    int64_t v0 = sim.now();
    const int64_t forBase = sim.now();
    bool stopped = false, lagWarned = false;
    auto closeClient = [&](size_t k, const char* why) {
        if (opt.verbose) std::cerr << "s7forge: " << clients[k]->peer << " closed (" << why << ")\n";
        S7F_CLOSE(clients[k]->s);
        clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(k));
    };
    for (;;) {
        if (stop && *stop) break;
        // the simulation up to now (real time times the speed), at most 20 ms
        // of work before the network is looked at again
        const auto workStart = clock::now();
        const double wall = std::chrono::duration<double>(workStart - t0).count();
        const int64_t target = v0 + static_cast<int64_t>(wall * opt.speed * 1e9);
        while (sim.now() < target && clock::now() - workStart < std::chrono::milliseconds(20)) {
            if (sim.running()) sim.scan();
            else sim.runFor(target - sim.now());  // in STOP time passes, nothing runs
        }
        // too slow for real time: the time it cannot catch up is dropped
        if (static_cast<double>(target - sim.now()) > kMaxLagSeconds * opt.speed * 1e9) {
            if (!lagWarned) {
                std::cerr << "s7forge: the simulation is slower than real time (times " << opt.speed
                          << "): it runs as fast as it can; a lower --speed or a longer --scan-ms helps\n";
                lagWarned = true;
            }
            v0 = sim.now() - static_cast<int64_t>(std::chrono::duration<double>(clock::now() - t0).count() * opt.speed * 1e9);
        }
        if (!sim.running() && !stopped) {
            stopped = true;
            std::cerr << "s7forge: the CPU went to STOP: " << sim.fault() << " (the server goes on answering)\n";
        }
        if (opt.forNs > 0 && sim.now() - forBase >= opt.forNs) break;
        // wait for the network up to the next cycle
        const double nextWall = static_cast<double>(sim.now() + sim.scanTime() - v0) / (opt.speed * 1e9);
        int waitMs = static_cast<int>((nextWall - std::chrono::duration<double>(clock::now() - t0).count()) * 1000);
        waitMs = std::max(0, std::min(waitMs, 50));
        std::vector<pollfd_t> fds;
        pollfd_t lp{};
        lp.fd = ls;
        lp.events = POLLIN;
        fds.push_back(lp);
        for (const auto& c : clients) {
            pollfd_t p{};
            p.fd = c->s;
            p.events = static_cast<short>(POLLIN | (c->out.empty() ? 0 : POLLOUT));
            fds.push_back(p);
        }
        const int n = S7F_POLL(fds.data(), static_cast<unsigned long>(fds.size()), waitMs);
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            break;
        }
        const auto now = clock::now();
        // the clients first (the list may grow below)
        for (size_t k = clients.size(); k-- > 0;) {
            Client& c = *clients[k];
            const short re = fds[k + 1].revents;
            if (re & (POLLERR | POLLHUP | POLLNVAL)) {
                if (!(re & POLLIN)) {
                    closeClient(k, "gone");
                    continue;
                }
            }
            if (re & POLLIN) {
                uint8_t buf[1024];
                const auto got2 = recv(c.s, reinterpret_cast<char*>(buf), sizeof buf, 0);
                if (got2 == 0 || (got2 < 0 && !wouldBlock())) {
                    closeClient(k, "closed by the client");
                    continue;
                }
                if (got2 > 0) {
                    if (c.in.empty()) c.partSince = now;
                    c.in.insert(c.in.end(), buf, buf + got2);
                }
                // whole frames: the length is in the MBAP header
                bool drop = false;
                while (c.in.size() >= 6) {
                    const size_t need = 6 + ((static_cast<size_t>(c.in[4]) << 8) | c.in[5]);
                    if (need < 8 || need > 260) {
                        drop = true;
                        break;
                    }
                    if (c.in.size() < need) break;
                    std::vector<uint8_t> frame(c.in.begin(), c.in.begin() + static_cast<std::ptrdiff_t>(need));
                    c.in.erase(c.in.begin(), c.in.begin() + static_cast<std::ptrdiff_t>(need));
                    c.last = c.partSince = now;
                    bool close = false;
                    const std::vector<uint8_t> answer = server.handle(frame, close);
                    if (close) {
                        drop = true;
                        break;
                    }
                    if (opt.verbose) std::cerr << "s7forge: " << c.peer << " " << server.lastRequest() << "\n";
                    c.out.insert(c.out.end(), answer.begin(), answer.end());
                }
                if (drop || c.out.size() > 65536) {
                    closeClient(k, "not Modbus/TCP");
                    continue;
                }
            }
            if (!c.out.empty()) {
                const auto sent = send(c.s, reinterpret_cast<const char*>(c.out.data()), static_cast<int>(c.out.size()), kSendFlags);
                if (sent < 0 && !wouldBlock()) {
                    closeClient(k, "send failed");
                    continue;
                }
                if (sent > 0) c.out.erase(c.out.begin(), c.out.begin() + sent);
            }
            if (now - c.last > std::chrono::seconds(kIdleSeconds)) closeClient(k, "idle");
            else if (!c.in.empty() && now - c.partSince > std::chrono::seconds(kPartialSeconds))
                closeClient(k, "an incomplete frame");
        }
        // new clients
        if (fds[0].revents & POLLIN) {
            for (;;) {
                sockaddr_in from{};
                socklen_t fl = sizeof from;
                socket_t s = accept(ls, reinterpret_cast<sockaddr*>(&from), &fl);
                if (s == kNoSocket) break;
                char ip[64] = "?";
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                const std::string peer = std::string(ip) + ":" + std::to_string(ntohs(from.sin_port));
                if (static_cast<int>(clients.size()) >= opt.maxClients) {
                    if (opt.verbose) std::cerr << "s7forge: " << peer << " refused (too many clients)\n";
                    S7F_CLOSE(s);
                    continue;
                }
                nonBlocking(s);
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
#ifdef SO_NOSIGPIPE
                setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&yes), sizeof yes);
#endif
                auto c = std::make_unique<Client>();
                c->s = s;
                c->peer = peer;
                c->last = now;
                if (opt.verbose) std::cerr << "s7forge: " << peer << " connected\n";
                clients.push_back(std::move(c));
            }
        }
    }
    for (auto& c : clients) S7F_CLOSE(c->s);
    S7F_CLOSE(ls);
    return 0;
}

}  // namespace s7forge
