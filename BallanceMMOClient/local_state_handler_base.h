#pragma once

#include <asio/thread_pool.hpp>
#include <asio/post.hpp>
#include "client.h"
#include "game_state.h"

class local_state_handler_base {
public:
	virtual void poll_and_send_state(CK3dObject* ball) = 0;

	virtual void poll_and_send_state_forced(CK3dObject* ball) = 0;

	inline constexpr const TimedBallState& get_local_state() const { return local_ball_state_; }

	inline void set_ball_type(uint32_t type) {
		local_ball_state_.type = type;
		local_ball_state_changed_ = true;
	}

	local_state_handler_base(asio::thread_pool& pool, client* client_ptr, ILogger* logger):
		thread_pool_(pool), client_(client_ptr), logger_(logger) {};

protected:
	asio::thread_pool& thread_pool_;
	client* client_;
	ILogger* logger_;
	TimedBallState local_ball_state_{};
	std::atomic_bool local_ball_state_changed_ = true;
	int force_send_counter_ = 0;
	constexpr static int FORCE_SEND_COUNTER_MAX = 100; // approx 1.5 seconds at 66 Hz

	void assemble_and_send_state() {
		static_assert(sizeof(VxVector) == sizeof(bmmo::vec3));
		static_assert(sizeof(VxQuaternion) == sizeof(bmmo::quaternion));
		++force_send_counter_;
		const bool force_send = (force_send_counter_ >= FORCE_SEND_COUNTER_MAX);
		if (local_ball_state_changed_ || force_send) {
			bmmo::timed_ball_state_msg msg{};
			std::memcpy(&(msg.content), &local_ball_state_, sizeof(BallState));
			msg.content.timestamp = local_ball_state_.timestamp;
			client_->send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
			local_ball_state_changed_ = false;
			if (force_send) force_send_counter_ = 0;
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

	void assemble_and_send_state_forced() const {
		bmmo::timed_ball_state_msg msg{};
		std::memcpy(&(msg.content), &local_ball_state_, sizeof(BallState));
		msg.content.timestamp = local_ball_state_.timestamp;
		client_->send(msg, k_nSteamNetworkingSend_Reliable);
	}
};
