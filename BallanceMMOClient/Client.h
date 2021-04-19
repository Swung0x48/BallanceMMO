#pragma once
#include <cstdint>
#include <blcl_net.h>
#include <iostream>
#include <mutex>

enum class MsgType : uint32_t {
    ServerAccept,
    ServerDeny,
    ServerPing,
    MessageAll,
    ServerMessage,

    Game_BallState
};

class Client : public blcl::net::client_interface<MsgType> {
private:
public:
    void ping_server();

    inline void broadcast_message(blcl::net::message<MsgType>& msg) { 
        send(msg); 
    }
};
