#pragma once

#include <BML/BMLAll.h>
#include "client.h"
#include "CommandMMO.h"
#include <unordered_map>
#include <mutex>

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class BallanceMMOClient : public IMod {
public:
	BallanceMMOClient(IBML* bml) : IMod(bml),
		client_([this](ammo::common::owned_message<PacketType>& msg) { OnMessage(msg); })
	{}

	virtual CKSTRING GetID() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetVersion() override { return "2.0.0-alpha1"; }
	virtual CKSTRING GetName() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetAuthor() override { return "Swung0x48"; }
	virtual CKSTRING GetDescription() override { return "The client to connect your game to the universe."; }
	DECLARE_BML_VERSION;

private:
	void OnLoad() override;
	void OnExitGame() override;
	void OnUnload() override;
	void OnMessage(ammo::common::owned_message<PacketType>& msg);

	std::unordered_map<std::string, IProperty*> props_;
	std::mutex bml_mtx_;
	client client_;
};