#pragma once
#include "bml_includes.h"
#include "common.hpp"

class CommandMMO: public ICommand
{
protected:
	std::function<void(IBML* bml, const std::vector<std::string>& args)> execute_callback_;
	std::function<std::vector<std::string>(IBML* bml, const std::vector<std::string>& args)> tab_callback_;
	void transform_ansi(std::vector<std::string>& strings) {
		std::ranges::transform(strings, strings.begin(),
													 [](const auto& s) { return bmmo::string_utils::utf8_to_ansi(s); });
	}

public:
	CommandMMO(decltype(execute_callback_) execute_callback, decltype(tab_callback_) tab_callback):
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
		auto completions = tab_callback_(bml, args);
		transform_ansi(completions);
		return completions;
	};
};

class CommandMMOSay: public CommandMMO
{
private:
	const std::vector<std::string> insert_mmo_prefix(std::vector<std::string> args) {
		args.insert(args.begin(), "mmo");
		return args;
	}
public:
	using CommandMMO::CommandMMO;
	virtual std::string GetName() override { return "say"; };
	virtual std::string GetAlias() override { return "s"; };
	virtual std::string GetDescription() override { return "BallanceMMO - alias for /ballancemmo say."; };
	virtual bool IsCheat() override { return false; };

	virtual void Execute(IBML* bml, const std::vector<std::string>& args) override {
		return CommandMMO::Execute(bml, insert_mmo_prefix(args));
	}
	virtual const std::vector<std::string> GetTabCompletion(IBML* bml, const std::vector<std::string>& args) override {
		return CommandMMO::GetTabCompletion(bml, insert_mmo_prefix(args));
	};
};
