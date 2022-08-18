#pragma once
#include <BML/BMLAll.h>

class CommandMMO: public ICommand
{
private:
	std::function<void(IBML* bml, const std::vector<std::string>& args)> execute_callback_;
	std::function<const std::vector<std::string>(IBML* bml, const std::vector<std::string>& args)> tab_callback_;
public:
	CommandMMO(std::function<void(IBML* bml, const std::vector<std::string>& args)> execute_callback, std::function<const std::vector<std::string>(IBML* bml, const std::vector<std::string>& args)> tab_callback):
		execute_callback_(std::move(execute_callback)), tab_callback_(std::move(tab_callback))
	{}
	virtual std::string GetName() override { return "ballancemmo"; };
	virtual std::string GetAlias() override { return "mmo"; };
	virtual std::string GetDescription() override { return "Commands for BallanceMMO."; };
	virtual bool IsCheat() override { return false; };

	virtual void Execute(IBML* bml, const std::vector<std::string>& args) override {
		execute_callback_(bml, args);
	}
	virtual const std::vector<std::string> GetTabCompletion(IBML* bml, const std::vector<std::string>& args) override {
		return tab_callback_(bml, args);
	};
};

class CommandMMOSay: public ICommand
{
private:
	std::function<void(IBML* bml, const std::vector<std::string>& args)> callback_;
public:
	CommandMMOSay(std::function<void(IBML* bml, const std::vector<std::string>& args)> callback):
		callback_(std::move(callback))
	{}
	virtual std::string GetName() override { return "say"; };
	virtual std::string GetAlias() override { return "s"; };
	virtual std::string GetDescription() override { return "BallanceMMO - alias for /ballancemmo say."; };
	virtual bool IsCheat() override { return false; };

	virtual void Execute(IBML* bml, const std::vector<std::string>& args) override {
		std::vector<std::string> new_args(args);
		new_args.insert(new_args.begin(), "mmo");
		callback_(bml, new_args);
	}
	virtual const std::vector<std::string> GetTabCompletion(IBML* bml, const std::vector<std::string>& args) override {
		return std::vector<std::string>{};
	};
};
