// ─────────────────────────────────────────────────────────────────────────────
//  se.net over loopback, against the generated UdpLoop: a UdpSender streaming
//  to a UdpReceiver, and a UdpClient talking to a UdpServer that echoes what
//  it receives back to whoever sent it.
//
//  What a host can rely on:
//
//    1. A sender's datagrams arrive, in order, the counter rising.
//    2. A client's datagrams reach the server, and the server's replies come
//       back to the client: full duplex, over one connected socket.
//    3. The server knows who it is talking to: `peer` is the client's address
//       and `peer_port` its port, and before anyone has written both are
//       empty and the server sends nothing.
//    4. A sender with no host, a port out of range, and a server on a port
//       already taken all stop the run at init().
//
//  Built and run by tests/run.ps1, from the emitted UdpLoop directory. Exit
//  status 0 means every check passed.
// ─────────────────────────────────────────────────────────────────────────────
#include "UdpLoop.cpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "udp_host: FAIL %s\n", what.c_str());
    ++failures;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string str(const char* p) { return p ? std::string(p) : std::string("(null)"); }

using Sim = udploop::Sim;

}  // namespace

int main() {
    {
        Sim sim;
        sim.log_capture(true);
        const int k = sim.channel_handle("src.out.k");
        const int rx = sim.channel_handle("dec_rx.out.v");
        const int cli = sim.channel_handle("dec_cli.out.v");
        const int srv = sim.channel_handle("dec_srv.out.v");
        const int peer = sim.channel_handle("srv.out.peer");
        const int port = sim.channel_handle("srv.out.peer_port");
        check(k >= 0 && rx >= 0 && cli >= 0 && srv >= 0 && peer >= 0 && port >= 0,
              "every channel takes a handle: " + std::string(sim.error()));
        check(sim.init(), "init: " + std::string(sim.error()));

        // Before anyone has written, the server has no peer.
        sim.tick();
        check(str(sim.read_text(peer)).empty(), "no peer before anyone has written: " +
                                                   str(sim.read_text(peer)));
        check(sim.read(port) == 0.0, "and no peer port");
        check(sim.read(cli) == -1.0, "and nothing has come back to the client");

        int last_rx = -1, rises = 0;
        bool in_order = true;
        while (!sim.done()) {
            // Loopback is fast but not instant: give each datagram a moment to
            // land before the next tick drains the socket.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            sim.tick();
            const int v = static_cast<int>(sim.read(rx));
            if (v >= 0) {
                if (v < last_rx) in_order = false;
                if (v > last_rx) ++rises;
                last_rx = v;
            }
        }
        const int sent = static_cast<int>(sim.read(k));
        check(in_order, "the receiver's counter never goes backwards");
        check(rises > sent / 2, "most of the sender's datagrams arrived: " + std::to_string(rises) +
                                    " of " + std::to_string(sent));
        check(last_rx > sent - 20, "the receiver keeps up: last " + std::to_string(last_rx) +
                                       ", sent " + std::to_string(sent));
        const int at_srv = static_cast<int>(sim.read(srv));
        const int back = static_cast<int>(sim.read(cli));
        check(at_srv > sent - 20, "the client's datagrams reach the server: " + std::to_string(at_srv));
        check(back > sent - 20 && back <= at_srv, "the server's echo comes back to the client: " +
                                                      std::to_string(back));
        check(str(sim.read_text(peer)) == "127.0.0.1", "the server's peer is the client's address: " +
                                                          str(sim.read_text(peer)));
        check(sim.read(port) == 47004.0, "and its port, the client's local_port");
        check(sim.finish() == 0, "a clean run");
        std::string log;
        Sim::LogLine line;
        while (sim.log_next(line)) log += line.text + "\n";
        check(!has(log, "failed"), "no send or receive failed:\n" + log);
    }

    // ── 4: what stops a run at init() ────────────────────────────────────────
    {
        Sim sim;
        sim.log_capture(true);
        check(sim.set("tx.host", ""), "an empty host is a valid value");
        check(!sim.init(), "but a sender with no host stops the run at init()");
        check(has(sim.error(), "UdpSender has no host to send to"), "naming why: " +
                                                                    std::string(sim.error()));
        check(sim.finish() != 0, "and the run is not clean");
    }
    {
        Sim sim;
        sim.log_capture(true);
        check(sim.set("srv.port", 70000.0), "a port out of range is an int");
        check(!sim.init(), "but the server refuses it");
        check(has(sim.error(), "1 <= port <= 65535 at srv"), "as a contract: " +
                                                             std::string(sim.error()));
        sim.finish();
    }
    {
        Sim first;
        first.log_capture(true);
        check(first.init(), "a first run binds the ports");
        Sim second;
        second.log_capture(true);
        check(!second.init(), "a second, on the same ports, cannot bind");
        check(has(second.error(), "is not available: bind"), "and says so: " +
                                                             std::string(second.error()));
        second.finish();
        first.finish();
    }

    if (failures == 0) std::printf("udp_host: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
