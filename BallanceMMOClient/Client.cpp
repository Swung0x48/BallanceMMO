#include "Client.h"

void Client::ping_server()
{
    blcl::net::message<MsgType> msg;
    msg.header.id = MsgType::ServerPing;

    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    msg << now;
    send(msg);
}