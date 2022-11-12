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
#include <boost/algorithm/string.hpp>

struct BallState {
	uint32_t type = 0;
	VxVector position{};
	VxQuaternion rotation{};
};

struct TimedBallState : BallState {
	SteamNetworkingMicroseconds timestamp{};

	TimedBallState(const bmmo::ball_state& state) {
		std::memcpy(this, &state, sizeof(BallState));
	}
	TimedBallState(const bmmo::timed_ball_state& state = {}) {
		std::memcpy(this, &state, sizeof(BallState));
		timestamp = static_cast<int64_t>(state.timestamp);
	};
};

struct PlayerState {
	std::string name;
	bool cheated = false;
	boost::circular_buffer<TimedBallState> ball_state = decltype(ball_state)(3, TimedBallState());
	SteamNetworkingMicroseconds time_diff = std::numeric_limits<decltype(time_diff)>::min();
	std::string current_map_name;
	int32_t current_sector = 0;
	int32_t current_sector_timestamp = 0;
	// BallState ball_state;

	// use linear extrapolation to get current position and rotation
	static inline const std::pair<VxVector, VxQuaternion> get_linear_extrapolated_state(const SteamNetworkingMicroseconds tc, const TimedBallState& state1, const TimedBallState& state2) {
		const auto time_interval = state2.timestamp - state1.timestamp;
		if (time_interval == 0)
			return { state2.position, state2.rotation };

		const auto factor = static_cast<float>(tc - state1.timestamp) / time_interval;

		return { state1.position + (state2.position - state1.position) * factor, Slerp(factor, state1.rotation, state2.rotation) };
	}

	// quadratic extrapolation of position (extrapolation for rotation is still linear)
	static inline const std::pair<VxVector, VxQuaternion> get_quadratic_extrapolated_state(const SteamNetworkingMicroseconds tc, const TimedBallState& state1, const TimedBallState& state2, const TimedBallState& state3) {
		const auto t21 = state2.timestamp - state1.timestamp,
			t32 = state3.timestamp - state2.timestamp,
			t31 = state3.timestamp - state1.timestamp;
		if (t32 == 0 || t31 == 0) return {state3.position, state3.rotation};
		if (t21 == 0) return get_linear_extrapolated_state(tc, state2, state3);

		return {
			state1.position * (((tc - state2.timestamp) * (tc - state3.timestamp)) / static_cast<double>(t21 * t31))
			+ state2.position * (((tc - state1.timestamp) * (tc - state3.timestamp)) / -static_cast<double>(t21 * t32))
			+ state3.position * (((tc - state1.timestamp) * (tc - state2.timestamp)) / static_cast<double>(t31 * t32)),
			Slerp(static_cast<float>(tc - state2.timestamp) / t32, state2.rotation, state3.rotation)
		};
	}
};

class game_state
{
public:
	bool create(HSteamNetConnection id, const std::string& name = "", bool cheated = false) {
		if (exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_.insert({id, {.name = name, .cheated = cheated}});
		return true;
	}

	std::optional<const PlayerState> get(HSteamNetConnection id) {
		if (!exists(id))
			return {};
		return states_[id];
	}

	std::optional<const PlayerState> get_from_nickname(std::string name) {
		auto id = get_client_id(name);
		if (id == k_HSteamNetConnection_Invalid)
			return {};
		return states_[id];
	}

	bool exists(HSteamNetConnection id) const {
		// std::shared_lock lk(mutex_);

		return states_.contains(id);
	}

	void remove_earlier_states(HSteamNetConnection id) {
		auto& state = states_[id].ball_state;
		state.push_front(state.front());
		state.push_front(state.front());
	}

	SteamNetworkingMicroseconds get_updated_timestamp(const HSteamNetConnection id, const SteamNetworkingMicroseconds timestamp) {
		// We have to assign a recalibrated timestamp here to reduce
		// errors caused by lags for our extrapolation to work.
		// Not setting new timestamps can get us almost accurate
		// real-time position of our own spirit balls, but everyone
		// has a different timestamp, so we have to account for this
		// and record everyone's average timestamp differences.
		states_[id].time_diff =
			(states_[id].time_diff == std::numeric_limits<decltype(states_[id].time_diff)>::min())
			? (SteamNetworkingUtils()->GetLocalTimestamp() - timestamp)
			: (PREV_DIFF_WEIGHT * states_[id].time_diff
				 - timestamp + SteamNetworkingUtils()->GetLocalTimestamp()) / (PREV_DIFF_WEIGHT + 1);
		// Weighted average - more weight on the previous value means more resistance
		// to random lag spikes, which in turn results in overall smoother movement;
		// however this also makes initial values converge into actual timestamp
		// differences slower and cause prolonged random flickering when average
		// lag values changed. We have to pick a value comfortable to both aspects.
		return (timestamp + states_[id].time_diff);
	};

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

	bool update(HSteamNetConnection id, TimedBallState&& state) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		state.timestamp = get_updated_timestamp(id, state.timestamp);
		if (state.timestamp <= states_[id].ball_state.front().timestamp)
			return true;
		states_[id].ball_state.push_front(state);
		return true;
	}

	bool update(HSteamNetConnection id, SteamNetworkingMicroseconds timestamp) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		TimedBallState last_state_copy(states_[id].ball_state.front());
		timestamp = get_updated_timestamp(id, timestamp);
		if (timestamp <= last_state_copy.timestamp)
			return true;
		last_state_copy.timestamp = timestamp;
		states_[id].ball_state.push_front(last_state_copy);
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

	bool update_map(HSteamNetConnection id, const std::string& map_name, const int32_t sector) {
		if (!exists(id))
			return false;
		std::unique_lock lk(mutex_);
		auto& state = states_[id];
		state.current_map_name = map_name;
		state.current_sector = sector;
		state.current_sector_timestamp = int32_t((SteamNetworkingUtils()->GetLocalTimestamp() - 3e12) / 1024);
		return true;
	}

	bool update_sector(HSteamNetConnection id, const int32_t sector) {
		if (!exists(id))
			return false;
		std::unique_lock lk(mutex_);
		states_[id].current_sector = sector;
		states_[id].current_sector_timestamp = int32_t((SteamNetworkingUtils()->GetLocalTimestamp() - 3e12) / 1024);
		return true;
	}

	bool update_sector_timestamp(HSteamNetConnection id, const int32_t sector_timestamp) {
		if (!exists(id))
			return false;
		std::unique_lock lk(mutex_);
		states_[id].current_sector_timestamp = sector_timestamp;
		return true;
	}

	void reset_time_data() {
		std::unique_lock lk(mutex_);
		for (auto& i : states_) {
			i.second.time_diff = std::numeric_limits<decltype(i.second.time_diff)>::min();
			for (auto& j : i.second.ball_state) {
				j.timestamp = std::numeric_limits<decltype(j.timestamp)>::min();
			}
		}
	}

	bool remove(HSteamNetConnection id) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_.erase(id);
		return true;
	}

	size_t player_count() {
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

	HSteamNetConnection get_client_id(std::string name) {
		auto it = std::find_if(states_.begin(), states_.end(),
													 [&name](const auto& s) { return boost::iequals(s.second.name, name); });
		if (it == states_.end())
			return k_HSteamNetConnection_Invalid;
		return it->first;
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
	HSteamNetConnection assigned_id_ = k_HSteamNetConnection_Invalid;
	std::atomic_bool nametag_visible_ = true;
	std::atomic_bool pending_cheat_flush_ = false;
	static constexpr inline const int64_t PREV_DIFF_WEIGHT = 15;
};
