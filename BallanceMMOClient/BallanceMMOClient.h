#pragma once

#include <BML/BMLAll.h>
#include "Client.h"
#include <concurrent_unordered_map.h>
#include <unordered_map>
#include <map>

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class BallanceMMOClient : public IMod {
public:
	BallanceMMOClient(IBML* bml) : IMod(bml) {}

	virtual CKSTRING GetID() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetVersion() override { return "0.1.15"; }
	virtual CKSTRING GetName() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetAuthor() override { return "Swung0x48"; }
	virtual CKSTRING GetDescription() override { return "The client to connect your game to the universe."; }
	DECLARE_BML_VERSION;
	
private:
	//struct SpiritBall {
		//CK3dObject* obj = nullptr;
		//std::vector<CKMaterial*> materials;
	//};

	struct BallState {
		uint32_t type = 0;
		VxVector position;
		VxQuaternion rotation;
	};

	const size_t MSG_MAX_SIZE = 25;
	Client client_;
	bool receiving_msg_ = false;
	std::thread msg_receive_thread_;
	blcl::net::message<MsgType> msg_ = blcl::net::message<MsgType>();
	CK3dObject* player_ball_ = nullptr;
	//CK3dObject* spirit_ball_ = nullptr;
	BallState ball_state_;
	//VxVector position_;
	//VxQuaternion rotation_;
	CKDataArray* current_level_array_ = nullptr;
	std::unordered_map<std::string, uint32_t> ball_name_to_idx_;
	CK3dObject* template_balls_[3];
	BGui::Gui* gui_ = nullptr;
	bool gui_avail_ = false;
	BGui::Label* ping_text_ = nullptr;
	char ping_char_[50];
	std::mutex ping_char_mtx_;
	long long loop_count_;
	std::mutex start_receiving_mtx;
	std::condition_variable start_receiving_cv_;
	bool ready_to_rx_ = false;

	struct PeerState {
		CK3dObject* balls[3] = { nullptr };
		uint32_t current_ball = 0;
	};
	concurrency::concurrent_unordered_map<uint32_t, PeerState> peer_balls_;
	std::unordered_map<std::string, IProperty*> props_;

	virtual void OnLoad() override;
	virtual void OnPostStartMenu() override;
	virtual void OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass,
		BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic,
		XObjectArray* objArray, CKObject* masterObj) override;
	virtual void OnProcess() override;
	virtual void OnStartLevel() override;
	virtual void OnUnload() override;
	virtual void OnBallNavActive() override;

private:
	CK3dObject* get_current_ball() { 
		if (current_level_array_)
			return static_cast<CK3dObject*>(current_level_array_->GetElementObject(0, 1));

		return nullptr;
	}

	void process_incoming_message(blcl::net::message<MsgType>& msg);
	CK3dObject* init_spirit_ball(int ball_index, uint32_t id);
};