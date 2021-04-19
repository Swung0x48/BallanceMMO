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
	virtual CKSTRING GetVersion() override { return "0.0.1"; }
	virtual CKSTRING GetName() override { return "BallanceMMOCLient"; }
	virtual CKSTRING GetAuthor() override { return "Swung0x48"; }
	virtual CKSTRING GetDescription() override { return "The Client to connect your game to the universe."; }
	DECLARE_BML_VERSION;

private:
	Client* client_ = nullptr;
	std::mutex write_msg_mtx_;
	blcl::net::message<MsgType> msg_ = blcl::net::message<MsgType>();
	CK3dObject* player_ball_ = nullptr;
	VxVector position_;
	VxQuaternion rotation_;

	virtual void OnPostStartMenu() override;
	virtual void OnProcess() override;
	virtual void OnStartLevel() override;
};