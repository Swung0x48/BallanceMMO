#pragma once

#include <BML/BMLAll.h>
#include "client.h"
#include "CommandMMO.h"
#include <unordered_map>
#include <mutex>
#include <memory>

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class BallanceMMOClient : public IMod {
public:
	BallanceMMOClient(IBML* bml) : IMod(bml),
		client_([this](ammo::common::owned_message<PacketType>& msg) { OnMessage(msg); }),
		ping_(bml_mtx_),
		status_(bml_mtx_)
	{}

	virtual CKSTRING GetID() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetVersion() override { return "2.0.0-alpha1"; }
	virtual CKSTRING GetName() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetAuthor() override { return "Swung0x48"; }
	virtual CKSTRING GetDescription() override { return "The client to connect your game to the universe."; }
	DECLARE_BML_VERSION;

private:
	struct text_sprite {
		std::unique_ptr<BGui::Text> sprite_;
		std::mutex mtx_;
		std::mutex& bml_mtx_;

		text_sprite(std::mutex& bml_mtx) : bml_mtx_(bml_mtx) {};
		text_sprite(const text_sprite&) = delete; // explicitly delete copy constructor

		bool update(const std::string& text, bool preemptive = true) {
			std::unique_lock bml_lk(bml_mtx_);
			if (!preemptive) {
				std::unique_lock lk(mtx_, std::try_to_lock);
				if (lk)
					sprite_.get()->SetText(text.c_str());
			}
			else {
				std::unique_lock lk(mtx_);
				sprite_.get()->SetText(text.c_str());
			}
		}
	};

	void OnLoad() override;
	void OnPreStartMenu() override;
	void OnExitGame() override;
	void OnUnload() override;
	void OnProcess() override;
	void OnMessage(ammo::common::owned_message<PacketType>& msg);

	std::unordered_map<std::string, IProperty*> props_;
	std::mutex bml_mtx_;
	client client_;

	text_sprite ping_;
	text_sprite status_;
};