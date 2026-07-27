#pragma once

#include "local_state_handler_base.h"

class player_state_handler : public local_state_handler_base {
private:
	// Reads the ball's CK object, so this runs on the game thread; the state it
	// writes is shared with the console and network threads, hence the lock.
	void poll_player_ball_state(CK3dObject* player_ball) {
		VxVector position; VxQuaternion rotation;
		player_ball->GetPosition(&position);
		player_ball->GetQuaternion(&rotation);
		const auto timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
		std::lock_guard lk(state_mutex_);
		if (position != local_ball_state_.position || rotation != local_ball_state_.rotation) {
			memcpy(&local_ball_state_.position, &position, sizeof(VxVector));
			memcpy(&local_ball_state_.rotation, &rotation, sizeof(VxQuaternion));
			local_ball_state_changed_ = true;
		}
		local_ball_state_.timestamp = timestamp;
	}

public:
	void poll_and_send_state(CK3dObject* ball) override {
		poll_player_ball_state(ball);
		// the message is assembled here and only the finished copy goes to the
		// pool, so the pool never reads state we are still writing
		assemble_and_send_state();
	};

	void poll_and_send_state_forced(CK3dObject* ball) override {
		poll_player_ball_state(ball);
		assemble_and_send_state_forced();
	};

	using local_state_handler_base::local_state_handler_base;
};

class spectator_state_handler : public local_state_handler_base {
public:
	void poll_and_send_state(CK3dObject* ball) override {}; // dummy method

	void poll_and_send_state_forced(CK3dObject* ball) override {
		{
			std::lock_guard lk(state_mutex_);
			local_ball_state_.timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
		}
		assemble_and_send_state_forced();
	};

	spectator_state_handler(asio::thread_pool& pool, client* client_ptr, ILogger* logger):
			local_state_handler_base(pool, client_ptr, logger) {
		local_ball_state_.position = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0.0f};
	};
};
