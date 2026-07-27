#pragma once

#include <mutex>

#include <asio/thread_pool.hpp>
#include <asio/post.hpp>
#include "client.h"
#include "game_state.h"

// Owns the local player's ball state.
//
// Threading: poll_and_send_state[_forced]() read the ball's CK object and must
// therefore be called on the game thread. Everything they touch afterwards is
// guarded by state_mutex_, because the console thread also reads the state
// (`getpos`) and the network thread reads it when rebuilding our own spirit
// ball. Messages are assembled on the calling thread and only the finished
// message is posted to the pool, so no thread but the owner ever looks at
// local_ball_state_ - the old code memcpy'd it on a pool thread while the game
// thread was overwriting it for the next frame.
class local_state_handler_base {
public:
	virtual void poll_and_send_state(CK3dObject* ball) = 0;

	virtual void poll_and_send_state_forced(CK3dObject* ball) = 0;

	// by value: a reference would let the caller read the state while another
	// thread is writing it
	TimedBallState get_local_state() const {
		std::lock_guard lk(state_mutex_);
		return local_ball_state_;
	}

	void set_ball_type(uint32_t type) {
		std::lock_guard lk(state_mutex_);
		local_ball_state_.type = type;
		local_ball_state_changed_ = true;
	}

	local_state_handler_base(asio::thread_pool& pool, client* client_ptr, ILogger* logger):
		thread_pool_(pool), client_(client_ptr), logger_(logger) {};

	virtual ~local_state_handler_base() = default;

protected:
	asio::thread_pool& thread_pool_;
	client* client_;
	ILogger* logger_;
	mutable std::mutex state_mutex_; // guards the three members below
	TimedBallState local_ball_state_{};
	bool local_ball_state_changed_ = true;
	int force_send_counter_ = 0;
	constexpr static int FORCE_SEND_COUNTER_MAX = 100; // approx 1.5 seconds at 66 Hz

	void assemble_and_send_state() {
		static_assert(sizeof(VxVector) == sizeof(bmmo::vec3));
		static_assert(sizeof(VxQuaternion) == sizeof(bmmo::quaternion));
		bmmo::timed_ball_state_msg msg{};
		bool send_full_state = false;
		SteamNetworkingMicroseconds timestamp{};
		{
			std::lock_guard lk(state_mutex_);
			++force_send_counter_;
			const bool force_send = (force_send_counter_ >= FORCE_SEND_COUNTER_MAX);
			send_full_state = local_ball_state_changed_ || force_send;
			if (send_full_state) {
				std::memcpy(&(msg.content), &local_ball_state_, sizeof(BallState));
				msg.content.timestamp = local_ball_state_.timestamp;
				local_ball_state_changed_ = false;
				if (force_send) force_send_counter_ = 0;
			}
			else {
				timestamp = local_ball_state_.timestamp;
			}
		}
		if (send_full_state) {
			asio::post(thread_pool_, [this, msg] {
				client_->send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
			});
#ifdef DEBUG
			logger_->Info("(%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f, %.2f)",
										msg.content.position.x, msg.content.position.y, msg.content.position.z,
										msg.content.rotation.x, msg.content.rotation.y,
										msg.content.rotation.z, msg.content.rotation.w);
#endif // DEBUG
		}
		else {
			asio::post(thread_pool_, [this, timestamp] {
				bmmo::timestamp_msg ts_msg{};
				ts_msg.content = timestamp;
				client_->send(ts_msg, k_nSteamNetworkingSend_UnreliableNoDelay);
			});
		}
	}

	void assemble_and_send_state_forced() {
		bmmo::timed_ball_state_msg msg{};
		{
			std::lock_guard lk(state_mutex_);
			std::memcpy(&(msg.content), &local_ball_state_, sizeof(BallState));
			msg.content.timestamp = local_ball_state_.timestamp;
		}
		asio::post(thread_pool_, [this, msg] {
			client_->send(msg, k_nSteamNetworkingSend_Reliable);
		});
	}
};
