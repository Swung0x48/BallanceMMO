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
	bool create(HSteamNetConnection id) {
		if (exists(id))
			return false;

		std::unique_lock lk(mutex_);
		peers_[id] = {};
		return true;
	}

	bool exists(HSteamNetConnection id) {
		std::shared_lock lk(mutex_);

		return peers_.find(id) != peers_.end();
	}

	bool update(HSteamNetConnection id, const std::string& name) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		peers_[id].name = name;
	}

	bool update(HSteamNetConnection id, const PlayerState& state) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		peers_[id] = state;
	}

	bool update(HSteamNetConnection id, const BallState& state) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		peers_[id].ball_state = state;
	}

	bool remove(HSteamNetConnection id) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		peers_.erase(id);
		return true;
	}

	size_t player_count(HSteamNetConnection id) {
		std::shared_lock lk(mutex_);
		return peers_.size();
	}
private:
	std::shared_mutex mutex_;
	std::unordered_map<HSteamNetConnection, PlayerState> peers_;
};
