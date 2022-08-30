#pragma once

#include <BML/BMLAll.h>
#include <asio/thread_pool.hpp>
#include <asio/post.hpp>
#include "client.h"
#include "game_state.h"

class local_state_handler_interface {
public:
	virtual void poll_and_send_state(CK3dObject* old_ball, CK3dObject* ball) = 0;

	virtual void poll_and_send_state_forced(CK3dObject* ball) = 0;

	inline TimedBallState& get_local_state() { return local_ball_state_; }

	inline void set_ball_type(uint32_t type) {
		local_ball_state_.type = type;
		local_ball_state_changed_ = true;
	}

	local_state_handler_interface(asio::thread_pool& pool, IBML* bml, client* client_ptr, ILogger* logger, game_state& db) :
		thread_pool_(pool), bml_(bml), client_(client_ptr), logger_(logger), db_(db) {};

protected:
	asio::thread_pool& thread_pool_;
	IBML* bml_;
	client* client_;
	ILogger* logger_;
	TimedBallState local_ball_state_{};
	std::atomic_bool local_ball_state_changed_ = true;
	game_state& db_;

	void assemble_and_send_state() {
		static_assert(sizeof(VxVector) == sizeof(bmmo::vec3));
		static_assert(sizeof(VxQuaternion) == sizeof(bmmo::quaternion));
		if (local_ball_state_changed_) {
			bmmo::timed_ball_state_msg msg{};
			std::memcpy(&(msg.content), &local_ball_state_, sizeof(BallState));
			msg.content.timestamp = local_ball_state_.timestamp;
			client_->send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
			local_ball_state_changed_ = false;
		}
		else {
			bmmo::timestamp_msg msg{};
			msg.content = local_ball_state_.timestamp;
			client_->send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
		}
#ifdef DEBUG
		logger_->Info("(%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f, %.2f)",
									local_ball_state_.position.x,
									local_ball_state_.position.y,
									local_ball_state_.position.z,
									local_ball_state_.rotation.x,
									local_ball_state_.rotation.y,
									local_ball_state_.rotation.z,
									local_ball_state_.rotation.w
		);
#endif // DEBUG
	}

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

	void assemble_and_send_state_forced() const {
		bmmo::timed_ball_state_msg msg{};
		std::memcpy(&(msg.content), &local_ball_state_, sizeof(BallState));
		msg.content.timestamp = local_ball_state_.timestamp;
		client_->send(msg, k_nSteamNetworkingSend_Reliable);
	}
};
