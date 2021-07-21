#include <iostream>
#include <AMMOProtocol.hpp>
#include "common.hpp"
#include <atomic>

class SimpleClient: public ammo::role::client<PacketType> {
public:
    void send_request() override {
        ammo::common::message<PacketType> msg;
        msg.header.id = ConnectionRequest;
        send(msg);
    }

    void disconnect() override {
        ammo::common::message<PacketType> msg;
        msg.header.id = ClientDisconnect;
        for (int i = 0; i < 10; ++i) {
            send(msg);
        }
        client::disconnect();
    }
    std::string name = "Spectator001";
};

int main() {
    SimpleClient client;
    if (!client.connect("127.0.0.1", 50000)) {
        std::cerr << "[ERR] Connect to server failed.\n";
    } else {
        std::cout << "[INFO] Connection request sent to server. Waiting for reply..." << std::endl;
    }

    std::atomic_bool will_quit = false;
    std::thread update_thread([&]() {
        while (!will_quit) {
            while (client.get_state() == ammo::role::client_state::Pending) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (client.get_incoming_messages().empty())
                    client.send_request();
                else {
                    auto msg = client.get_incoming_messages().pop_front();
                    if (msg.message.header.id == ConnectionAccepted) {
                        client.confirm_validation();
                        std::cout << "[INFO] Accepted by server!" << std::endl;
                    } else if (msg.message.header.id == ConnectionChallenge) {
                        uint64_t checksum; msg.message >> checksum;
                        checksum = encode_for_validation(checksum);
                        msg.message.clear();
                        msg.message << checksum;
                        ammo::entity::string<PacketType> name = client.name;
                        name.serialize(msg.message);
                        msg.message.header.id = ConnectionResponse;
                        client.send(msg.message);
                    } else if (msg.message.header.id == Denied) {
                        std::cout << "[WARN] Rejected by server." << std::endl;
                    }
                }
            }
            if (client.connected()) {
                if (!client.get_incoming_messages().empty()) {
                    auto msg = client.get_incoming_messages().pop_front().message;
                    switch (msg.header.id) {
                        case PacketFragment: {
                            break;
                        }
                        case Denied: {
                            break;
                        }
                        case Ping: {
                            auto now = std::chrono::system_clock::now().time_since_epoch().count();
                            uint64_t then; msg >> then;
                            auto ping = now - then; // in microseconds
                            std::cout << "[INFO] Ping: " << ping / 1000 << " ms" << std::endl;
                            break;
                        }
                        case GameState: {
                            break;
                        }
                        default: {
                            std::cout << "[WARN] Unknown message ID: " << msg.header.id << std::endl;
                        }
                    }
                }
            }
        }
    });

    while (true) {
        int a; std::cin >> a;
        if (a == 1) {
            ammo::common::message<PacketType> msg;
            msg.header.id = Ping;
            auto now = std::chrono::system_clock::now().time_since_epoch().count();
            msg << now;
            client.send(msg);
        } else if (a == 2) {
            will_quit = true;
            break;
        }
    }
    client.disconnect();
    if (update_thread.joinable())
        update_thread.join();
}
