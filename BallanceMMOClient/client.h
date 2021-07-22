#pragma once
#include <AMMOProtocol.hpp>
#include "../BallanceMMOServer/common.hpp"
class client: public ammo::role::client<PacketType>
{
	std::function<void(ammo::common::owned_message<PacketType>& msg)> callback_;
public:
	client(std::function<void(ammo::common::owned_message<PacketType>& msg)> callback):
		callback_(std::move(callback)) {
	}

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

    void on_message(ammo::common::owned_message<PacketType>& msg) override {
		callback_(msg);
    }
};

