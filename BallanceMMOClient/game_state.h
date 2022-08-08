#pragma once
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif
#include <BML/BMLAll.h>

#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <boost/circular_buffer.hpp>

struct BallState {
	uint32_t type = 0;
	VxVector position{};
	VxQuaternion rotation{};
};

struct TimedBallState : BallState {
	int64_t timestamp = 0;
};

struct PlayerState {
	std::string name;
	bool cheated = false;
	boost::circular_buffer<TimedBallState> ball_state;
	SteamNetworkingMicroseconds time_diff = 0;
	// BallState ball_state;

	PlayerState(): ball_state(3) {
		ball_state.assign(3, TimedBallState());
	}

	// use linear extrapolation to get current position and rotation
	static inline const std::pair<VxVector, VxQuaternion> get_linear_extrapolated_state(const TimedBallState& first, const TimedBallState& second) {
		const auto time_interval = second.timestamp - first.timestamp;
		if (time_interval == 0)
			return { second.position, second.rotation };

		const auto factor = static_cast<double>(SteamNetworkingUtils()->GetLocalTimestamp() - second.timestamp) / time_interval;

		return { second.position + (second.position - first.position) * factor, second.rotation + (second.rotation - first.rotation) * factor };
	}

	// quadratic extrapolation
	static inline const std::pair<VxVector, VxQuaternion> get_quadratic_extrapolated_state(const TimedBallState& first, const TimedBallState& second, const TimedBallState& third) {
		const auto t21 = second.timestamp - first.timestamp,
			t32 = third.timestamp - second.timestamp,
			t31 = third.timestamp - first.timestamp;
		if (t32 == 0) return {third.position, third.rotation};
		if (t21 == 0) return get_linear_extrapolated_state(third, second);

		const auto tc = SteamNetworkingUtils()->GetLocalTimestamp();

		const auto f1 = ((tc - second.timestamp) * (tc - third.timestamp)) / static_cast<double>(t21 * t31),
			f2 = ((tc - first.timestamp) * (tc - third.timestamp)) / static_cast<double>(t21 * t32),
			f3 = ((tc - first.timestamp) * (tc - second.timestamp)) / static_cast<double>(t31 * t32);

		return { first.position * f1 - second.position * f2 + third.position * f3, first.rotation * f1 - second.rotation * f2 + third.rotation * f3 };
	}
};

class game_state
{
public:
	bool create(HSteamNetConnection id, const std::string& name = "", bool cheated = false) {
		if (exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_[id] = {};
		states_[id].name = name;
		states_[id].cheated = cheated;
		return true;
	}

	std::optional<const PlayerState> get(HSteamNetConnection id) {
		if (!exists(id))
			return {};
		return states_[id];
	}

	bool exists(HSteamNetConnection id) {
		std::shared_lock lk(mutex_);

		return states_.find(id) != states_.end();
	}

	bool update(HSteamNetConnection id, const std::string& name) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_[id].name = name;
		set_pending_flush(true);
		return true;
	}

	bool update(HSteamNetConnection id, const PlayerState& state) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_[id] = state;
		return true;
	}

	bool update(HSteamNetConnection id, TimedBallState& state) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		// We have to assign a new timestamp here to reduce
		// errors caused by lags for our extrapolation to work.
		// Not setting new timestamps can get us almost accurate
		// real-time position of our own spirit balls, but everyone
		// has a different timestamp, so we have to account for this.
		states_[id].time_diff = (states_[id].time_diff + SteamNetworkingUtils()->GetLocalTimestamp() - state.timestamp) / 2;
		state.timestamp += states_[id].time_diff;
		if (state.timestamp < states_[id].ball_state.back().timestamp)
			return true;
		states_[id].ball_state.push_back(state);
		return true;
	}

	bool update(HSteamNetConnection id, bool cheated) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_[id].cheated = cheated;
		set_pending_flush(true);
		return true;
	}

	bool remove(HSteamNetConnection id) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_.erase(id);
		return true;
	}

	size_t player_count(HSteamNetConnection id) {
		std::shared_lock lk(mutex_);
		return states_.size();
	}

	template <class Fn>
	void for_each(const Fn& fn) {
		using pair_type = std::pair<const HSteamNetConnection, PlayerState>;
		constexpr bool is_arg_const = std::is_invocable_v<Fn, const pair_type&>;
		using argument_type = std::conditional_t<is_arg_const, const pair_type, pair_type>&;

		// lock
		if constexpr (is_arg_const) {
			mutex_.lock_shared();
		} else {
			mutex_.lock();
		}

		// looping
		for (argument_type i : states_) {
			if (!fn(i))
				break;
		}

		// unlock
		if constexpr (is_arg_const) {
			mutex_.unlock_shared();
		}
		else {
			mutex_.unlock();
		}
	}

	void clear() {
		std::unique_lock lk(mutex_);
		states_.clear();
	}

	void set_nickname(const std::string& name) {
		std::unique_lock lk(mutex_);
		nickname_ = name;
	}

	std::string get_nickname() {
		std::shared_lock lk(mutex_);
		return nickname_;
	}

	void set_client_id(HSteamNetConnection id) {
		std::unique_lock lk(mutex_);
		assigned_id_ = id;
	}

	HSteamNetConnection get_client_id() {
		return assigned_id_;
	}

	void set_ball_id(const std::string& name, const uint32_t id) {
		ball_name_to_id_[name] = id;
	}

	uint32_t get_ball_id(const std::string& name) {
		return ball_name_to_id_[name];
	}

	bool is_nametag_visible() {
		return nametag_visible_;
	}

	void set_nametag_visible(bool visible) {
		nametag_visible_ = visible;
	}

	void toggle_nametag_visible() {
		nametag_visible_ = !nametag_visible_;
	}

	void set_pending_flush(bool flag) {
		pending_cheat_flush_ = flag;
	}

	bool get_pending_flush() {
		return pending_cheat_flush_;
	}

	bool flush() {
		bool f = pending_cheat_flush_;
		pending_cheat_flush_ = false;
		return f;
	}
private:
	std::shared_mutex mutex_;
	std::unordered_map<HSteamNetConnection, PlayerState> states_;
	std::unordered_map<std::string, uint32_t> ball_name_to_id_; 
	std::string nickname_;
	HSteamNetConnection assigned_id_;
	std::atomic_bool nametag_visible_ = true;
	std::atomic_bool pending_cheat_flush_ = false;
};
