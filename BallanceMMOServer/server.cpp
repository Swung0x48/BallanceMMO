#include <iostream>
#include <AMMOProtocol.hpp>
#include <atomic>
#include <unordered_map>
#include "common.hpp"

class SimpleServer: public ammo::role::server<PacketType> {
public:
    explicit SimpleServer(uint16_t port): ammo::role::server<PacketType>(port) {}
    std::unordered_map<asio::ip::udp::endpoint, PlayerData> online_clients_;
    static std::string get_current_time_string() {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::string ret = std::ctime(&now);
        return ret.substr(0, ret.length() - 1);
    }

    void broadcast_message(const ammo::common::owned_message<PacketType>& msg) {
        std::for_each(online_clients_.begin(), online_clients_.end(),[this, &msg](auto& item) {
            auto& [key, value] = item;
            if (key != msg.remote && value.state == ammo::role::client_state::Connected) {
                ammo::common::owned_message<PacketType> owned_msg = { key, msg.message };
                send(owned_msg);
            }
        });
    }

    void cleanup() {
        std::for_each(online_clients_.begin(), online_clients_.end(),[](auto& item) {
            auto& [key, value] = item;
            // 1-minute timeout (no update timeout)
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - value.last_timestamp) > std::chrono::minutes(1)) {
#ifdef DEBUG
                std::cout << "[DEBUG] Kicking " << value.name << '(' << value.id << ", " << key << ")\n";
                std::cout << "[DEBUG] Last sequence: " << value.last_sequence << '\n';
                std::cout << "[DEBUG] Last timestamp: " << value.last_timestamp.time_since_epoch().count() << '\n';
                std::cout << "[DEBUG] -----\n";
                std::cout << "[DEBUG] Current time: " << std::chrono::system_clock::now().time_since_epoch().count()<< std::endl;
#endif
                if (value.state == ammo::role::client_state::Connected)
                    std::cout << '[' << SimpleServer::get_current_time_string() << "] [INFO] " << value.name << " left the game. (Timeout)" << std::endl;
                else if (value.state == ammo::role::client_state::Pending)
                    std::cout << '[' << SimpleServer::get_current_time_string() << "] [INFO] (" << key << ") disconnected. (Timeout)" << std::endl;
                value.state = ammo::role::client_state::Disconnected;
            }
        });

        std::erase_if(online_clients_, [](const auto& item) {
            auto const& [key, value] = item;
#ifdef DEBUG
            std::cout << "[DEBUG] Purging " << value.name << '(' << value.id << ", " << key << ")\n";
#endif
            return value.state == ammo::role::client_state::Disconnected;
        });
    }
protected:
    void on_message(ammo::common::owned_message<PacketType>& msg) override {
        if (online_clients_.contains(msg.remote)) {
            if (PlayerData::sequence_greater_than(online_clients_[msg.remote].last_sequence, msg.message.header.sequence))
                return;
            online_clients_[msg.remote].last_sequence = msg.message.header.sequence;
            online_clients_[msg.remote].last_timestamp = std::chrono::system_clock::now();
        }

        switch (msg.message.header.id) {
            case PacketFragment: {
                break;
            }
            case ConnectionRequest: {
                std::cout << '[' << SimpleServer::get_current_time_string() << "] [INFO] Connection request from " << msg.remote << '\n';
                std::cout << '[' << SimpleServer::get_current_time_string() << "] [INFO] Assigned ID: " << next_id_ << std::endl;
                uint64_t checksum = std::chrono::system_clock::now().time_since_epoch().count();
                online_clients_[msg.remote] = {
                        next_id_++,
                        "",
                        encode_for_validation(checksum),
                        msg.message.header.sequence,
                        std::chrono::system_clock::now(),
                        ammo::role::client_state::Pending
                };
                msg.message.clear();
                msg.message << checksum;
                msg.message.header.id = ConnectionChallenge;
                send(msg);
                break;
            }
            case ConnectionResponse: {
                uint64_t response; msg.message >> response;
                auto& client = online_clients_[msg.remote];

                bool denied = false;
#ifdef DEBUG
                std::cout << "[DEBUG] Checking client validity..." << '\n';
                std::cout << "[DEBUG] expected: " << client.checksum << '\n';
                std::cout << "[DEBUG] actual: " << response << std::endl;
#endif
                if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - client.last_timestamp) > std::chrono::minutes(1)) {
                    // 1-minute timeout (request-to-response)
                    denied = true;
#ifdef DEBUG
                    std::cout << "[DEBUG] last timestamp: " << client.last_timestamp.time_since_epoch().count() << '\n';
                    std::cout << "[DEBUG] current time: " << std::chrono::system_clock::now().time_since_epoch().count()<< std::endl;
#endif
                    std::cout << '[' << SimpleServer::get_current_time_string() << "] [WARN] Denied connection from " << msg.remote << " (Timeout)" << std::endl;
                }
                if (response != client.checksum) {
                    std::cout << '[' << SimpleServer::get_current_time_string() << "] [WARN] Denied connection from " << msg.remote << " (Challenge-response failed)" << std::endl;
                    denied = true;
                }
                if (denied) {
                    // Set its state to Disconnected
                    client.state = ammo::role::client_state::Disconnected;

                    // Reply to client
                    msg.message.clear();
                    msg.message.header.id = Denied;
                    send(msg);
                    break;
                }

                client.checksum = 0;
                // Set its state to Connected
                client.state = ammo::role::client_state::Connected;

                // Read in username
                ammo::entity::string<PacketType> username;
                username.deserialize(msg.message);
                client.name = username.str;
                std::cout << '[' << SimpleServer::get_current_time_string() << "] [INFO] " << client.name << " joined the game." << std::endl;

                // Reply with ConnectionAccepted
                msg.message.clear();
                msg.message.header.id = ConnectionAccepted;
                msg.message << client.id;
                send(msg);

                // Send already online clients
                msg.message.clear();
                msg.message.header.id = OnlineClients;
                uint32_t count = 0; // Just pad, come back later.
                msg.message << count;
                try {
                    for (auto& client : online_clients_) {
                        if (client.second.state == ammo::role::client_state::Connected) {
                            msg.message << client.second.id;
                            ammo::entity::string<PacketType> name = client.second.name;
                            name.serialize(msg.message);
                            ++count;
                        }
                    }
                    msg.message.write_position = 0;
                    msg.message << count;
                    if (msg.message.body.size() <= MAX_PACKET_SIZE) {
                        send(msg);
                    }
                }
                catch (const std::runtime_error& e) {
                    std::cout << '[' << SimpleServer::get_current_time_string() << "] [ERR] " << e.what() << std::endl;
                }

                // Broadcast to other online clients
                msg.message.clear();
                msg.message.header.id = ClientConnected;
                msg.message << client.id;
                username.serialize(msg.message);
                broadcast_message(msg);
                break;
            }
            case Ping: {
                if (online_clients_.contains(msg.remote) &&
                    online_clients_.at(msg.remote).state == ammo::role::client_state::Connected)
                    send(msg);
                break;
            }
            case ClientDisconnect: {
                if (online_clients_.contains(msg.remote) && online_clients_[msg.remote].state != ammo::role::client_state::Disconnected) {
                    auto& client = online_clients_.at(msg.remote);
                    client.state = ammo::role::client_state::Disconnected;
                    std::cout << '[' << SimpleServer::get_current_time_string() << "] [INFO] " << ((!client.name.empty()) ? client.name : ("(" + msg.remote.address().to_string() + ":" + std::to_string(msg.remote.port()) + ")")) << " left the game. (On ClientDisconnect)" << std::endl;
                }
                break;
            }
            case GameState: {
                //std::cout << "On GameState " << msg.remote << std::endl;
                broadcast_message(msg);
                break;
            }
            default: {
                std::cout << '[' << SimpleServer::get_current_time_string() << "] [WARN] Unexpected message ID: " << msg.message.header.id << std::endl;
            }
        }

        if (online_clients_.contains(msg.remote)) {
            online_clients_[msg.remote].last_sequence = msg.message.header.sequence;
        }
    }
private:
    uint64_t next_id_ = 0;
};

int main() {
#ifdef DEBUG
    std::cout << "[DEBUG] Now running under development mode." << std::endl;
#endif
    SimpleServer server(50000);
    if (server.start())
        std::cout << '[' << SimpleServer::get_current_time_string() << "] [INFO] Server started!\n";
    std::atomic_bool updating = true;

    std::thread update_thread([&server, &updating] () {
        while (updating) {
            server.update(64, true, std::chrono::minutes(5));
            server.cleanup();
        }
    });

    while (true) {
        std::string cmd;
        std::cin >> cmd;
        if (cmd == "/stop") {
            server.stop();
            updating = false;
            server.tick();
            if (update_thread.joinable())
                update_thread.join();
            return 0;
        }
    }
}