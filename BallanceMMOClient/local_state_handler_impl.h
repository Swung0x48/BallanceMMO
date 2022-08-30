#pragma once

#include "local_state_handler_interface.h"

class player_state_handler : public local_state_handler_interface {
public:
	void poll_and_send_state(CK3dObject* old_ball, CK3dObject* ball) override {
		poll_player_ball_state(ball);
		// Check on trafo after polling states.
		// We have to keep track of whether our ball state is changed.

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

	player_state_handler(asio::thread_pool& pool, IBML* bml, client* client_ptr, ILogger* logger, game_state& db):
			local_state_handler_interface(pool, bml, client_ptr, logger, db) {};
};

class spectator_state_handler : public local_state_handler_interface {
public:
	void poll_and_send_state(CK3dObject* old_ball, CK3dObject* ball) override {}; // dummy method

	void poll_and_send_state_forced(CK3dObject* ball) override {
		local_ball_state_.timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
		asio::post(thread_pool_, [this]() {
			assemble_and_send_state_forced();
		});
	};

	spectator_state_handler(asio::thread_pool& pool, IBML* bml, client* client_ptr, ILogger* logger, game_state& db):
			local_state_handler_interface(pool, bml, client_ptr, logger, db) {
		local_ball_state_.position = { 1048576.0f, 1048576.0f, 1048576.0f };
	};
};
