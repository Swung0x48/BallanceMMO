#pragma once

#include <BML/BMLAll.h>
#include "Client.h"

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class BallanceMMOClient : public IMod {
public:
	BallanceMMOClient(IBML* bml) : IMod(bml) {}

	virtual CKSTRING GetID() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetVersion() override { return "0.0.3"; }
	virtual CKSTRING GetName() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetAuthor() override { return "Swung0x48"; }
	virtual CKSTRING GetDescription() override { return "The client to connect your game to the universe."; }
	DECLARE_BML_VERSION;

private:
	struct SpiritBall {
		CK3dObject* obj;
		std::vector<CKMaterial*> materials;
	};

	const size_t MSG_MAX_SIZE = 500;
	Client client_;
	std::thread msg_thread_;
	blcl::net::message<MsgType> msg_ = blcl::net::message<MsgType>();
	blcl::net::tsqueue<blcl::net::message<MsgType>> msg_queue_;
	CK3dObject* player_ball_ = nullptr;
	CK3dObject* spirit_ball_ = nullptr;
	VxVector position_;
	VxQuaternion rotation_;
	CKDataArray* current_level_array_;
	std::unordered_map<std::string, SpiritBall> spirit_balls_;

	virtual void OnLoad() override;
	virtual void OnPostStartMenu() override;
	virtual void OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass,
		BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic,
		XObjectArray* objArray, CKObject* masterObj) override;
	virtual void OnProcess() override;
	virtual void OnStartLevel() override;
	virtual void OnUnload() override;

	CK3dObject* get_current_ball() { 
		if (current_level_array_)
			return static_cast<CK3dObject*>(current_level_array_->GetElementObject(0, 1));

		return nullptr;
	}

	void process_incoming_message(const blcl::net::owned_message<MsgType>& msg);
};