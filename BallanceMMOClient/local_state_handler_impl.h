#pragma once

#include "local_state_handler_interface.h"

class player_state_handler : public local_state_handler_interface {
private:
	void poll_player_ball_state(CK3dObject* player_ball) {
		VxVector position; VxQuaternion rotation;
		player_ball->GetPosition(&position);
		player_ball->GetQuaternion(&rotation);
		if (position != local_ball_state_.position || rotation != local_ball_state_.rotation) {
			memcpy(&local_ball_state_.position, &position, sizeof(VxVector));
			memcpy(&local_ball_state_.rotation, &rotation, sizeof(VxQuaternion));
			local_ball_state_changed_ = true;
		}
		local_ball_state_.timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
	}

public:
	void poll_and_send_state(CK3dObject* ball) override {
		poll_player_ball_state(ball);
		asio::post(thread_pool_, [this]() {
			assemble_and_send_state();
		});
	};

	void poll_and_send_state_forced(CK3dObject* ball) override {
		poll_player_ball_state(ball);
		asio::post(thread_pool_, [this]() {
			assemble_and_send_state_forced();
		});
	};

	using local_state_handler_interface::local_state_handler_interface;
};

class spectator_state_handler : public local_state_handler_interface {
public:
	void poll_and_send_state(CK3dObject* ball) override {}; // dummy method

	void poll_and_send_state_forced(CK3dObject* ball) override {
		local_ball_state_.timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
		asio::post(thread_pool_, [this]() {
			assemble_and_send_state_forced();
		});
	};

	spectator_state_handler(asio::thread_pool& pool, client* client_ptr, ILogger* logger):
			local_state_handler_interface(pool, client_ptr, logger) {
		local_ball_state_.position = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0.0f};
	};
};
