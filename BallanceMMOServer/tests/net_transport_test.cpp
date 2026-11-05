// The simulated transport (net/sim_transport.hpp, step K2): what the session
// code relies on from a transport - the connection's life, reliable order,
// seeded unreliable loss, and a lingering close that arrives after its data.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <net/sim_transport.hpp>

namespace {

using namespace bmmo::net;

struct seen {
    std::vector<status_change> changes;
    std::vector<std::string> messages;
};

void pump(transport& t, seen& out) {
    t.receive([&](const message& m) { out.messages.emplace_back(static_cast<const char*>(m.data), m.size); }, 1024);
    t.poll_status([&](const status_change& c) { out.changes.push_back(c); });
}

TEST(SimTransport, ConnectAcceptSendClose) {
    sim_network net;
    auto server = net.make_transport(), client = net.make_transport();
    ASSERT_TRUE(server->listen(26995));
    const connection c = client->connect("127.0.0.1:26995");
    ASSERT_NE(c, kInvalidConnection);
    seen s, k;
    pump(*server, s);
    ASSERT_EQ(s.changes.size(), 1u);
    EXPECT_EQ(s.changes[0].state, connection_state::connecting);
    const connection on_server = s.changes[0].conn;
    EXPECT_EQ(server->accept(on_server), transport::accept_result::ok);
    pump(*server, s);
    pump(*client, k);
    EXPECT_EQ(s.changes.back().state, connection_state::connected);
    EXPECT_EQ(k.changes.back().state, connection_state::connected);

    EXPECT_TRUE(client->send(c, "hello", 5, kSendReliable));
    pump(*server, s);
    ASSERT_EQ(s.messages, std::vector<std::string>{"hello"});

    client->close(c, kEndApp + 1, "bye", true);
    pump(*server, s);
    EXPECT_EQ(s.changes.back().state, connection_state::closed_by_peer);
    EXPECT_EQ(s.changes.back().end_reason, kEndApp + 1);
    EXPECT_EQ(s.changes.back().end_debug, "bye");
}

TEST(SimTransport, ReliableInOrderAfterLatencyAndLingeringClose) {
    sim_network net;
    auto server = net.make_transport(), client = net.make_transport();
    server->listen(1);
    const connection c = client->connect(":1");
    seen s, k;
    pump(*server, s);
    server->accept(s.changes[0].conn);
    pump(*client, k);
    client->set_fake_lag(transport::direction::send, 50);
    client->send(c, "a", 1, kSendReliable);
    client->set_fake_lag(transport::direction::send, 10);   // a later, faster send still arrives second
    client->send(c, "b", 1, kSendReliable);
    client->close(c, 0, {}, true);
    net.advance(20'000);
    pump(*server, s);
    EXPECT_TRUE(s.messages.empty());
    EXPECT_EQ(s.changes.back().state, connection_state::connected);   // the close waits for its data
    net.advance(40'000);
    pump(*server, s);
    EXPECT_EQ(s.messages, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(s.changes.back().state, connection_state::closed_by_peer);
}

TEST(SimTransport, UnreliableLossFollowsTheSeed) {
    const auto lost = [](uint64_t seed) {
        sim_network net(seed);
        auto server = net.make_transport(), client = net.make_transport();
        server->listen(1);
        const connection c = client->connect(":1");
        seen s, k;
        pump(*server, s);
        server->accept(s.changes[0].conn);
        client->set_fake_loss(transport::direction::send, 30.0f);
        for (int i = 0; i < 200; ++i) client->send(c, &i, sizeof(i), kSendUnreliableNoDelay);
        pump(*server, s);
        return s.messages;
    };
    const auto a = lost(7), b = lost(7), c = lost(8);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_GT(a.size(), 100u);
    EXPECT_LT(a.size(), 180u);
}

// Step LF: the coordinator's link events - a latency step, a blackout, bursty loss.
TEST(SimTransport, LinkStepBlackoutAndBursts) {
    sim_network net(3);
    auto server = net.make_transport(), client = net.make_transport();
    server->listen(1);
    const connection c = client->connect(":1");
    seen s, k;
    pump(*server, s);
    server->accept(s.changes[0].conn);
    pump(*client, k);
    sim_network::add_lag(*client, transport::direction::send, 50);
    client->send(c, "a", 1, kSendReliable);
    net.advance(49'000);
    pump(*server, s);
    EXPECT_TRUE(s.messages.empty());
    net.advance(1'000);
    pump(*server, s);
    EXPECT_EQ(s.messages, std::vector<std::string>{"a"});

    sim_network::blackout(*client, transport::direction::send, net.now() + 30'000);
    client->send(c, "u", 1, kSendUnreliableNoDelay);   // dropped
    client->send(c, "r", 1, kSendReliable);            // after the blackout plus the latency
    net.advance(79'000);
    pump(*server, s);
    EXPECT_EQ(s.messages.size(), 1u);
    net.advance(1'000);
    pump(*server, s);
    EXPECT_EQ(s.messages, (std::vector<std::string>{"a", "r"}));

    s.messages.clear();
    client->set_fake_loss(transport::direction::send, 10.0f);
    sim_network::set_loss_burst(*client, transport::direction::send, 8);
    for (int i = 0; i < 2000; ++i) client->send(c, &i, sizeof(i), kSendUnreliableNoDelay);
    net.advance(100'000);
    pump(*server, s);
    pump(*server, s);   // a pump takes 1024
    int last = -1;
    for (const auto& m: s.messages) {   // every hole a whole number of bursts
        int i = 0;
        std::memcpy(&i, m.data(), sizeof(i));
        EXPECT_EQ((i - last - 1) % 8, 0) << "hole before " << i;
        last = i;
    }
    EXPECT_GT(s.messages.size(), 1700u);
    EXPECT_LT(s.messages.size(), 1900u);
}

}

// The capture seams (session/capture_seams.hpp, step K3): what a recording
// transport and clock write, their replaying twins hand back in order.
#include <filesystem>

#include <session/capture_seams.hpp>

namespace {

TEST(CaptureSeams, RecordThenReplay) {
    const auto path = (std::filesystem::temp_directory_path() / "bmmo_capture_seams_test.bmic").string();
    const int64_t den = 1000000000;
    std::vector<std::string> live_messages, replayed_messages;
    std::vector<bmmo::net::connection_state> live_states, replayed_states;
    int live_ping = 0;
    int64_t live_read = 0;
    {
        bmmo::session::capture_file file;
        std::string error;
        ASSERT_TRUE(file.open_capture(path, "test", den, error)) << error;
        bmmo::net::sim_network net;
        auto server = net.make_transport();
        bmmo::session::recording_transport client(net.make_transport(), file);
        bmmo::session::recording_clock clock(bmmo::session::real_session_clock(), file);
        server->listen(1);
        const auto c = client.connect(":1");
        std::vector<bmmo::net::status_change> s;
        server->poll_status([&](const bmmo::net::status_change& x) { s.push_back(x); });
        server->accept(s[0].conn);
        server->send(s[0].conn, "hi", 2, bmmo::net::kSendReliable);
        client.receive([&](const bmmo::net::message& m) { live_messages.emplace_back(static_cast<const char*>(m.data), m.size); }, 16);
        client.poll_status([&](const bmmo::net::status_change& x) { live_states.push_back(x.state); });
        bmmo::net::connection_status status;
        client.status(c, status);
        live_ping = status.ping_ms;
        live_read = clock.now(bmmo::session::clock_site::journal_ms).time_since_epoch().count();
        file.close();
    }
    bmmo::session::capture_file file;
    std::string error;
    ASSERT_TRUE(file.open_replay(path, den, error)) << error;
    EXPECT_EQ(file.version(), bmmo::session::capture_file::kVersion);
    std::string diverged;
    const auto on_diverged = [&](const std::string& why) { diverged = why; };
    bmmo::session::replay_transport transport(file, on_diverged);
    bmmo::session::replay_clock clock(file, bmmo::session::real_session_clock(), on_diverged);
    transport.receive([&](const bmmo::net::message& m) { replayed_messages.emplace_back(static_cast<const char*>(m.data), m.size); }, 16);
    transport.poll_status([&](const bmmo::net::status_change& x) { replayed_states.push_back(x.state); });
    bmmo::net::connection_status status;
    transport.status(1, status);
    EXPECT_EQ(clock.now(bmmo::session::clock_site::journal_ms).time_since_epoch().count(), live_read);
    EXPECT_EQ(replayed_messages, live_messages);
    EXPECT_EQ(replayed_states, live_states);
    EXPECT_EQ(status.ping_ms, live_ping);
    EXPECT_TRUE(diverged.empty()) << diverged;
    EXPECT_TRUE(file.complete());
    clock.now(bmmo::session::clock_site::journal_ms);   // a read the live run did not make
    EXPECT_FALSE(diverged.empty());
    std::filesystem::remove(path);
}

// K7d: the retail mod's records - a queue's takes (empty ones too), a frame's
// keys, a send - come back where they were written; a free site is not written.
TEST(CaptureSeams, RetailRecords) {
    const auto path = (std::filesystem::temp_directory_path() / "bmmo_capture_retail_test.bmic").string();
    const int64_t den = 1000000000;
    {
        bmmo::session::capture_file file;
        std::string error;
        ASSERT_TRUE(file.open_capture(path, "retail", den, error)) << error;
        file.set_free(bmmo::session::clock_site::retail_display);
        bmmo::session::recording_clock clock(bmmo::session::real_session_clock(), file);
        file.take(1, {"start", "assign"});
        file.take(2, {});
        clock.now(bmmo::session::clock_site::retail_display);
        file.keys(7, "keys", 4);
        file.sent(2, "input");
        file.close();
    }
    bmmo::session::capture_file file;
    std::string error;
    ASSERT_TRUE(file.open_replay(path, den, error)) << error;
    std::vector<std::string> taken;
    EXPECT_FALSE(file.take_take(2, taken, error));   // the next take is site 1's
    ASSERT_TRUE(file.take_take(1, taken, error)) << error;
    EXPECT_EQ(taken, (std::vector<std::string>{"start", "assign"}));
    ASSERT_TRUE(file.take_take(2, taken, error)) << error;
    EXPECT_TRUE(taken.empty());
    std::string keys;
    EXPECT_FALSE(file.take_keys(6, keys));   // another frame's
    ASSERT_TRUE(file.take_keys(7, keys));
    EXPECT_EQ(keys, "keys");
    EXPECT_FALSE(file.take_sent(2, "other", error));
    EXPECT_TRUE(file.complete());
    std::filesystem::remove(path);
}

}

// Steps T1b, T1e: a coordinated run - two processes over TCP to a coordinator,
// each on its own virtual clock.  The client's link carries 10 ms each way: the
// connect reaches the server 10 ms after it was made, the accept the client 10 ms
// after that, the server sees a 20 ms round trip and the client's "hi" 10 ms
// after it was sent.  The run, done twice in lockstep and twice with the
// lookahead (each process running ahead by the link's latency), logs the same
// events at the same virtual times all four times, the lookahead in fewer rounds.
#include <algorithm>
#include <atomic>
#include <thread>

#include <net/coordinator.hpp>
#include <net/remote_transport.hpp>

namespace {

using namespace std::chrono_literals;

std::vector<std::string> coordinated_run(bool lockstep, uint64_t& rounds) {
    using bmmo::session::clock_site;
    bmmo::net::coordinator_options options;
    options.processes = 2;
    options.client_latency_ms = 10;
    options.until_us = options.start_us + 300000;
    options.lockstep = lockstep;
    bmmo::net::coordinator coordinator(options);
    const std::string address = "127.0.0.1:" + std::to_string(coordinator.port());
    std::string run_error;
    std::thread run([&] { coordinator.run(run_error); });

    std::mutex log_mutex;
    std::vector<std::string> log;
    const auto note = [&](const std::string& who, bmmo::session::virtual_clock& clock, const std::string& what) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(clock.now(clock_site::wait_pace).time_since_epoch()).count()
                      - options.start_us;
        std::lock_guard lk(log_mutex);
        log.push_back(who + " " + std::to_string(us) + " " + what);
    };
    struct side {
        bmmo::session::virtual_clock clock;
        bmmo::net::remote_transport transport{clock};
        std::atomic<bool> stop{false};
    };
    side server, client;
    // Concurrently, as two processes would: the welcome waits for every hello.
    std::string server_error, client_error;
    std::thread server_open([&] { EXPECT_TRUE(server.transport.open(address, 0, "server", server_error)) << server_error; });
    EXPECT_TRUE(client.transport.open(address, 1, "client", client_error)) << client_error;
    server_open.join();
    server.transport.start(1, [&] { server.stop = true; });
    client.transport.start(1, [&] { client.stop = true; });
    std::thread server_loop([&] {
        server.transport.listen(1);
        while (!server.stop) {
            server.clock.sleep_for(clock_site::server_loop_wait, 15ms);
            server.transport.poll_status([&](const bmmo::net::status_change& c) {
                if (c.state == bmmo::net::connection_state::connecting) server.transport.accept(c.conn);
                bmmo::net::connection_status st;
                const bool known = server.transport.status(c.conn, st);
                note("server", server.clock, "state " + std::to_string(static_cast<int>(c.state)) + " ping " + std::to_string(known ? st.ping_ms : -1));
            });
            server.transport.receive([&](const bmmo::net::message& m) {
                note("server", server.clock, "got " + std::string(static_cast<const char*>(m.data), m.size) + " received "
                     + std::to_string(m.received - options.start_us));
            }, 16);
        }
    });
    std::thread client_loop([&] {
        const auto c = client.transport.connect("127.0.0.1:1");
        bool connected = false, sent = false;
        while (!client.stop) {
            client.clock.sleep_for(clock_site::wait_pace, 5ms);
            client.transport.poll_status([&](const bmmo::net::status_change& s) {
                connected = connected || s.state == bmmo::net::connection_state::connected;
                note("client", client.clock, "state " + std::to_string(static_cast<int>(s.state)));
            });
            if (connected && !sent) {
                sent = client.transport.send(c, "hi", 2, bmmo::net::kSendReliable);
                note("client", client.clock, "sent hi");
            }
        }
    });
    run.join();
    server_loop.join();
    client_loop.join();
    EXPECT_TRUE(run_error.empty()) << run_error;
    rounds = coordinator.rounds();
    // The two processes log concurrently: each one's lines in its own order.
    std::stable_sort(log.begin(), log.end(), [](const std::string& a, const std::string& b) {
        return a.substr(0, a.find(' ')) < b.substr(0, b.find(' '));
    });
    return log;
}

TEST(Coordinator, TwoProcessesOneVirtualTimeTheSameRunInLockstepAndAhead) {
    uint64_t lockstep_rounds = 0, ahead_rounds = 0, ignored = 0;
    const auto first = coordinated_run(true, lockstep_rounds);
    EXPECT_EQ(coordinated_run(true, ignored), first);
    const auto ahead = coordinated_run(false, ahead_rounds);
    EXPECT_EQ(ahead, first);
    EXPECT_EQ(coordinated_run(false, ignored), first);
    EXPECT_GT(lockstep_rounds, 40u);
    EXPECT_LT(ahead_rounds * 3, lockstep_rounds * 2);   // 5 ms wakes, 10 ms ahead
    std::string all;
    for (const auto& line: first) all += line + '\n';
    // The connect, made at 0, reaches the server at +10 ms; its pass at +15 ms
    // accepts, which reaches the client at +25 ms; its "hi", sent then,
    // arrives at +35 ms and the server's pass at +45 ms takes it.
    EXPECT_NE(all.find("client 25000 sent hi"), std::string::npos) << all;
    EXPECT_NE(all.find("ping 20"), std::string::npos) << all;
    EXPECT_NE(all.find("server 45000 got hi received 35000"), std::string::npos) << all;
}

}
