#pragma once

#include <BML/BMLAll.h> 
#include <timercpp.h>
#include "Client.h"
#include <concurrent_unordered_map.h>
#include <unordered_map>
#include <map>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "sha256.h"

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class BallanceMMOClient : public IMod {
public:
	BallanceMMOClient(IBML* bml) : IMod(bml) {}

	virtual CKSTRING GetID() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetVersion() override { return "1.2.0-alpha14"; }
	virtual CKSTRING GetName() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetAuthor() override { return "Swung0x48"; }
	virtual CKSTRING GetDescription() override { return "The client to connect your game to the universe."; }
	DECLARE_BML_VERSION;
	
private:
	struct BallState {
		uint32_t type = 0;
		VxVector position;
		VxQuaternion rotation;
	};

	const size_t BUF_SIZE = 1024;
	const size_t MSG_MAX_SIZE = 512;
	const unsigned int SEND_BALL_STATE_INTERVAL = 15;
	const unsigned int PING_INTERVAL = 1000;
	const unsigned int PING_TIMEOUT = 2000;
	Client client_;
	bool receiving_msg_ = false;
	std::thread msg_receive_thread_;
	blcl::net::message<MsgType> msg_ = blcl::net::message<MsgType>();
	CK3dObject* player_ball_ = nullptr;
	BallState ball_state_;
	CKDataArray* current_level_array_ = nullptr;
	std::unordered_map<std::string, uint32_t> ball_name_to_idx_;
	CK3dObject* template_balls_[3];
	std::unique_ptr<BGui::Gui> gui_ = nullptr;
	bool gui_avail_ = false;
	bool connected_ = false;
	std::unique_ptr<BGui::Label> ping_text_;
	char ping_char_[50];
	std::mutex ping_char_mtx_;
	std::mutex start_receiving_mtx;
	std::condition_variable start_receiving_cv_;
	bool ready_to_rx_ = false;
	std::string map_hash_;
	bool show_player_name_ = false;

	//Timer send_ball_state_;
	Timer pinging_;

	struct PeerState {
		CK3dObject* balls[3] = { nullptr };
		uint32_t current_ball = 0;
		std::string player_name = "";
		std::unique_ptr<BGui::Label> username_label;
	};
	concurrency::concurrent_unordered_map<uint64_t, PeerState> peer_balls_;
	std::unordered_map<std::string, IProperty*> props_;

	void OnLoad() override;
	void OnPreStartMenu() override;
	void OnPostStartMenu() override;
	void OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass,
		BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic,
		XObjectArray* objArray, CKObject* masterObj) override;
	void OnProcess() override;
	void OnStartLevel() override;
	void OnUnload() override;
	void OnBallNavActive() override;
	void OnBallNavInactive() override;
	void OnPreExitLevel() override;
	void OnPreEndLevel() override;

private:
	CK3dObject* get_current_ball() { 
		if (current_level_array_)
			return static_cast<CK3dObject*>(current_level_array_->GetElementObject(0, 1));

		return nullptr;
	}

	void process_incoming_message(blcl::net::message<MsgType>& msg);
	CK3dObject* init_spirit_ball(int ball_index, uint64_t id);

	void add_active_client(uint64_t client, const std::string& player_name) {
		peer_balls_[client].player_name = player_name;
	}

	uint32_t crc32(std::ifstream& fs) {
		std::vector<char> buffer(BUF_SIZE, 0);
		uint32_t crc = 0;
		while (!fs.eof())
		{
			fs.read(buffer.data(), buffer.size());
			std::streamsize read_size = fs.gcount();
			CKComputeDataCRC(buffer.data(), read_size, crc);
		}

		return crc;
	}

	bool hide_player_ball(uint64_t client_id) {
		try {
			auto& peerstate = peer_balls_.at(client_id);
			if (peerstate.balls[peerstate.current_ball] == nullptr)
				return false;

			peerstate.balls[peerstate.current_ball]->Show(CKHIDE);
			peerstate.username_label->SetVisible(false);
			return true;
		}
		catch (std::out_of_range& e) {
			GetLogger()->Warn(e.what());
			return false;
		}
	}
};