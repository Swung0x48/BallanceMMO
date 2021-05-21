#pragma once
#include <cstdint>
#include <blcl_net.h>
#include <iostream>
#include <mutex>

enum class MsgType : uint32_t {
    ServerAccept,
    ServerDeny,
    ServerPing,
    ClientDisconnect,
    BallState,
    UsernameReq,
    Username,
    UsernameAck,
    EnterMap,
    FinishLevel,
    ExitMap,
    MapHashReq,
    MapHash,
    MapHashAck
};

class Client : public blcl::net::client_interface<MsgType> {
private:
public:
    uint32_t max_username_length_ = 0;

    void ping_server();

    inline void broadcast_message(const blcl::net::message<MsgType>& msg) { 
        send(msg); 
    }

    void send_username(const std::string& username) {
        blcl::net::message<MsgType> msg;
        msg.header.id = MsgType::Username;
        std::vector<uint8_t> username_bin(max_username_length_ + 1);
        strncpy(reinterpret_cast<char*>(username_bin.data()), username.c_str(), max_username_length_);
        msg.write(username_bin.data(), strlen(reinterpret_cast<char*>(username_bin.data())) + 1);
        send(msg);
    }
};


