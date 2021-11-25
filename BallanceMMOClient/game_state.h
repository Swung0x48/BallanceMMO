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
		states[id] = {};
		return true;
	}

	bool exists(HSteamNetConnection id) {
		std::shared_lock lk(mutex_);

		return states.find(id) != states.end();
	}

	bool update(HSteamNetConnection id, const std::string& name) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states[id].name = name;
	}

	bool update(HSteamNetConnection id, const PlayerState& state) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states[id] = state;
	}

	bool update(HSteamNetConnection id, const BallState& state) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states[id].ball_state = state;
	}

	bool remove(HSteamNetConnection id) {
		if (!exists(id))
			return false;

		std::unique_lock lk(mutex_);
		states.erase(id);
		return true;
	}

	size_t player_count(HSteamNetConnection id) {
		std::shared_lock lk(mutex_);
		return states.size();
	}

	auto read_lock() {
		return std::move(std::shared_lock(mutex_));
	}

	auto write_lock() {
		return std::move(std::unique_lock(mutex_));
	}

	std::unordered_map<HSteamNetConnection, PlayerState> states;

private:
	std::shared_mutex mutex_;
};
