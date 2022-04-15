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

struct BallState {
	uint32_t type = 0;
	VxVector position;
	VxQuaternion rotation;
};

struct PlayerState {
	std::string name;
	bool cheated = false;
	BallState ball_state;
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

	bool update(HSteamNetConnection id, bool cheated) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states_[id].cheated = cheated;
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
private:
	std::shared_mutex mutex_;
	std::unordered_map<HSteamNetConnection, PlayerState> states_;
	std::unordered_map<std::string, uint32_t> ball_name_to_id_; 
	std::string nickname_;
	HSteamNetConnection assigned_id_;
	std::atomic_bool nametag_visible_ = true;
};
