#pragma once
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif
#include <BML/BMLAll.h>

#include <unordered_map>
#include <shared_mutex>

struct BallState {
	uint32_t type = 0;
	VxVector position;
	VxQuaternion rotation;
};

struct PlayerState {
	std::string name;
	BallState ball_state;
};

class game_state
{
public:
	bool create(HSteamNetConnection id, const std::string& name = "") {
		if (exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_[id] = {};
		states_[id].name = name;
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
		return true;
	}

	bool update(HSteamNetConnection id, const PlayerState& state) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_[id] = state;
		return true;
	}

	bool update(HSteamNetConnection id, const BallState& state) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_[id].ball_state = state;
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

	void set_ball_id(const std::string& name, const uint32_t id) {
		ball_name_to_id_[name] = id;
	}

	uint32_t get_ball_id(const std::string& name) {
		return ball_name_to_id_[name];
	}
private:
	std::shared_mutex mutex_;
	std::unordered_map<HSteamNetConnection, PlayerState> states_;
	std::unordered_map<std::string, uint32_t> ball_name_to_id_; 
};
