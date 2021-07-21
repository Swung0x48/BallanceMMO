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
        ammo::role::client<PacketType>::disconnect();
    }

    std::string name = "Spectator001";
protected:
    void on_message(ammo::common::owned_message<PacketType> &msg) override {
        if (!connected()) {
            if (msg.message.header.id == ConnectionAccepted) {
                confirm_validation();
                std::cout << "[INFO] Accepted by server!" << std::endl;
            } else if (msg.message.header.id == ConnectionChallenge) {
                uint64_t checksum;
                msg.message >> checksum;
                checksum = encode_for_validation(checksum);
                msg.message.clear();
                msg.message << checksum;
                ammo::entity::string<PacketType> str = this->name;
                str.serialize(msg.message);
                msg.message.header.id = ConnectionResponse;
                send(msg.message);
            } else if (msg.message.header.id == Denied) {
                std::cout << "[WARN] Rejected by server." << std::endl;
            }
        } else {
            switch (msg.message.header.id) {
                case PacketFragment: {
                    break;
                }
                case Denied: {
                    break;
                }
                case Ping: {
                    auto now = std::chrono::system_clock::now().time_since_epoch().count();
                    uint64_t then; msg.message >> then;
                    auto ping = now - then; // in microseconds
                    std::cout << "[INFO] Ping: " << ping / 1000 << " ms" << std::endl;
                    break;
                }
                case GameState: {
                    break;
                }
                default: {
                    std::cout << "[WARN] Unknown message ID: " << msg.message.header.id << std::endl;
                }
            }
        }
    }
};

int main() {
    SimpleClient client;
    if (!client.connect("127.0.0.1", 50000)) {
        std::cerr << "[ERR] Connect to server failed.\n";
    } else {
        std::cout << "[INFO] Connection request sent to server. Waiting for reply..." << std::endl;
    }

    while (true) {
        int a; std::cin >> a;
        if (a == 1) {
            ammo::common::message<PacketType> msg;
            msg.header.id = Ping;
            auto now = std::chrono::system_clock::now().time_since_epoch().count();
            msg << now;
            client.send(msg);
        } else if (a == 2) {
            client.disconnect();
            client.shutdown();
            break;
        }
    }
}
