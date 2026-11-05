// The simulated transport (net/sim_transport.hpp, step K2): what the session
// code relies on from a transport - the connection's life, reliable order,
// seeded unreliable loss, and a lingering close that arrives after its data.

#include <gtest/gtest.h>

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

}
