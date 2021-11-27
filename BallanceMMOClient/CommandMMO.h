#pragma once
#include <BML/BMLAll.h>
#include <memory>
//#include "client.h"
//#include "text_sprite.h"
class CommandMMO: public ICommand
{
private:
	std::function<void(IBML* bml, const std::vector<std::string>& args)> callback_;
public:
	CommandMMO(std::function<void(IBML* bml, const std::vector<std::string>& args)> callback):
		callback_(std::move(callback))
	{}
	virtual std::string GetName() override { return "ballancemmo"; };
	virtual std::string GetAlias() override { return "mmo"; };
	virtual std::string GetDescription() override { return "Commands for BallanceMMO."; };
	virtual bool IsCheat() override { return false; };

	virtual void Execute(IBML* bml, const std::vector<std::string>& args) override {
		callback_(bml, args);
	}
	virtual const std::vector<std::string> GetTabCompletion(IBML* bml, const std::vector<std::string>& args) override {
		return args.size() == 2 ? std::vector<std::string>{ "connect", "disconnect", "help" } : std::vector<std::string>{};
	};
};

