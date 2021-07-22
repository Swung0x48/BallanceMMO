#pragma once
#include <BML/BMLAll.h>
#include <memory>
#include "client.h"
#include "text_sprite.h"
class CommandMMO: public ICommand
{
private:
	client& client_;
	std::unordered_map<std::string, IProperty*>& props_;
	std::mutex& bml_lock_;
	std::shared_ptr<text_sprite> ping_;
	std::shared_ptr<text_sprite> status_;

	void help(IBML* bml) {
		std::lock_guard<std::mutex> lk(bml_lock_);
		bml->SendIngameMessage("BallanceMMO Help");
		bml->SendIngameMessage("/connect - Connect to server.");
		bml->SendIngameMessage("/disconnect - Disconnect from server.");
	}

public:
	CommandMMO(client& client, std::unordered_map<std::string, IProperty*>& props, 
		std::mutex& bml_lock,
		std::shared_ptr<text_sprite> ping,
		std::shared_ptr<text_sprite> status):
		client_(client),
		props_(props),
		bml_lock_(bml_lock),
		ping_(ping),
		status_(status)
	{}
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
						status_->update("Pending");
						status_->paint(0xFFF6A71B);
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
						ping_->update("Ping: --- ms");
						status_->update("Disconnected");
						status_->paint(0xffff0000);
						std::lock_guard<std::mutex> lk(bml_lock_);
						bml->SendIngameMessage("Disconnected.");
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

