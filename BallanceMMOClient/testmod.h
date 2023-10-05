#pragma once

#include "bml_includes.h"
#include <map>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <format>
#include <ranges>
#include <filesystem>
#include <fstream>

//#include <imgui/imgui.h>

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class testmod : public IMod {
public:
	testmod(IBML* bml):
		IMod(bml)
	{
	}
	
	virtual BMMO_CKSTRING GetID() override { return "testmod"; }
	virtual BMMO_CKSTRING GetVersion() override { return "0.0.0"; }
	virtual BMMO_CKSTRING GetName() override { return "test"; }
	virtual BMMO_CKSTRING GetAuthor() override { return "Swung0x48"; }
	virtual BMMO_CKSTRING GetDescription() override { return "Test"; }
	DECLARE_BML_VERSION;
	
private:
	//void OnLoad() override;
	//void OnPostStartMenu() override;
	//void OnExitGame() override;
	////void OnUnload() override;
	void OnProcess() override;
	//void OnStartLevel() override;
	//void OnPostCheckpointReached() override;
	//void OnPostExitLevel() override;
	//void OnCounterActive() override;
	//void OnPauseLevel() override;
	//void OnBallOff() override;
	//void OnCamNavActive() override;
	//void OnPreLifeUp() override;
	//void OnLevelFinish() override;
	//void OnLoadScript(BMMO_CKSTRING filename, CKBehavior* script) override;
	//void OnCheatEnabled(bool enable) override;
	//void OnModifyConfig(BMMO_CKSTRING category, BMMO_CKSTRING key, IProperty* prop) override;
};
