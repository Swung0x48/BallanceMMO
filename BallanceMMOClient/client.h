#pragma once
#include <AMMOProtocol.hpp>
#include "../BallanceMMOServer/common.hpp"
class client: public ammo::role::client<PacketType>
{
public:
	void send_request() override {
		ammo::common::message<PacketType> msg;
		msg.header.id = ConnectionRequest;
		send(msg);
	}
	void disconnect() override {
		ammo::common::message<PacketType> msg;
		msg.header.id = ClientDisconnect;
		for (int i = 0; i < 20; ++i) {
			send(msg);
		}
		ammo::role::client<PacketType>::disconnect();
	}
};

