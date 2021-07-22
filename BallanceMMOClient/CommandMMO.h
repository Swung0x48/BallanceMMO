#pragma once
#include <BML/BMLAll.h>
#include "client.h"
class CommandMMO: public ICommand
{
private:
	client& client_;
	std::unordered_map<std::string, IProperty*>& props_;
	std::mutex& bml_lock_;

	void help(IBML* bml) {
		std::lock_guard<std::mutex> lk(bml_lock_);
		bml->SendIngameMessage("BallanceMMO Help");
		bml->SendIngameMessage("/connect - Connect to server.");
		bml->SendIngameMessage("/disconnect - Disconnect from server.");
	}

public:
	CommandMMO(client& client, std::unordered_map<std::string, IProperty*>& props, std::mutex& bml_lock):
		client_(client),
		props_(props),
		bml_lock_(bml_lock) {}
	virtual std::string GetName() override { return "ballancemmo"; };
	virtual std::string GetAlias() override { return "mmo"; };
	virtual std::string GetDescription() override { return "Commands for BallanceMMO."; };
	virtual bool IsCheat() override { return false; };

	virtual void Execute(IBML* bml, const std::vector<std::string>& args) override {
		switch (args.size()) {
			case 1: {
				help(bml);
				break;
			}
			case 2: {
				if (args[1] == "connect") {
					if (client_.connected()) {
						std::lock_guard<std::mutex> lk(bml_lock_);
						bml->SendIngameMessage("Already connected.");
					}
					else if (client_.get_state() == ammo::role::client_state::Disconnected) {
						std::lock_guard<std::mutex> lk(bml_lock_);
						if (client_.connect(props_["remote_addr"]->GetString(), props_["remote_port"]->GetInteger()))
							bml->SendIngameMessage("Connection request sent to server. Waiting for reply...");
						else
							bml->SendIngameMessage("Connect to server failed.");
					}
				}
				else if (args[1] == "disconnect") {
					if (client_.get_state() == ammo::role::client_state::Disconnected) {
						std::lock_guard<std::mutex> lk(bml_lock_);
						bml->SendIngameMessage("Already disconnected.");
					} else {
						client_.disconnect();
					}
				}
				break;
			}
			default: {
				help(bml);
			}
		}
	}
	virtual const std::vector<std::string> GetTabCompletion(IBML* bml, const std::vector<std::string>& args) override {
		return args.size() == 2 ? std::vector<std::string>{ "connect", "disconnect", "help" } : std::vector<std::string>{};
	};
};

