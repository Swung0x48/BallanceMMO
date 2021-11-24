#pragma once
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif
#include <BML/BMLAll.h>

#include <unordered_map>

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
	bool exists(HSteamNetConnection id) {
		return peers_.find(id) != peers_.end();
	}

	bool update(HSteamNetConnection id, const std::string& name) {
		peers_[id].name = name;
	}

	bool update(HSteamNetConnection id, const PlayerState& state) {
		peers_[id] = state;
	}

	void update(HSteamNetConnection id, const BallState& state) {
		peers_[id].ball_state = state;
	}

	bool remove(HSteamNetConnection id) {
		if (!exists(id))
			return false;

		peers_.erase(id);
		return true;
	}

	size_t player_count(HSteamNetConnection id) {
		return peers_.size();
	}
private:
	std::unordered_map<HSteamNetConnection, PlayerState> peers_;
};
