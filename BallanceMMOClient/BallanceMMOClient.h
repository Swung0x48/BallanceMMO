#pragma once

#include <BML/BMLAll.h>
#include "CommandMMO.h"
#include "text_sprite.h"
#include "label_sprite.h"
#include "client.h"
#include "game_state.h"
#include "game_objects.h"
#include "dumpfile.h"
#include <unordered_map>
#include <mutex>
#include <memory>
#include <format>
#include <asio.hpp>

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class BallanceMMOClient : public IMod, public client {
public:
	BallanceMMOClient(IBML* bml): 
		IMod(bml),
		objects_(bml, db_)
		//client_([this](ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) { LoggingOutput(eType, pszMsg); },
		//	[this](SteamNetConnectionStatusChangedCallback_t* pInfo) { OnConnectionStatusChanged(pInfo); })
	{
		this_instance_ = this;
	}

	bmmo::version_t version;
	string version_string = version.to_string();
	virtual CKSTRING GetID() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetVersion() override { return version_string.c_str(); }
	virtual CKSTRING GetName() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetAuthor() override { return "Swung0x48"; }
	virtual CKSTRING GetDescription() override { return "The client to connect your game to the universe."; }
	DECLARE_BML_VERSION;

	static void init_socket() {
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		SteamDatagramErrMsg err_msg;
		if (!GameNetworkingSockets_Init(nullptr, err_msg))
			FatalError("GameNetworkingSockets_Init failed.  %s", err_msg);
#else
		SteamDatagramClient_SetAppID(570); // Just set something, doesn't matter what
		//SteamDatagramClient_SetUniverse( k_EUniverseDev );

		SteamDatagramErrMsg errMsg;
		if (!SteamDatagramClient_Init(true, errMsg))
			FatalError("SteamDatagramClient_Init failed.  %s", errMsg);

		SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
#endif
		init_timestamp_ = SteamNetworkingUtils()->GetLocalTimestamp();
		SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, LoggingOutput);
	}

private:
	void OnLoad() override;
	void OnPostStartMenu() override;
	void OnExitGame() override;
	void OnUnload() override;
	void OnProcess() override;
	void OnStartLevel() override;
	void OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj) override;
	void OnLevelFinish() override;
	void OnLoadScript(CKSTRING filename, CKBehavior* script) override;
	void OnCheatEnabled(bool enable) override;
	// Custom
	void OnCommand(IBML* bml, const std::vector<std::string>& args);
	void OnTrafo(int from, int to);
	void OnPeerTrafo(uint64_t id, int from, int to);

	// Callbacks from client
	static void LoggingOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg);
	void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) override;
	void on_message(ISteamNetworkingMessage* network_msg) override;

	static void terminate(long delay);

	static void FatalError(const char* fmt, ...) {
		char text[2048];
		va_list ap;
		va_start(ap, fmt);
		vsprintf(text, fmt, ap);
		va_end(ap);
		char* nl = strchr(text, '\0') - 1;
		if (nl >= text && *nl == '\n')
			*nl = '\0';
		LoggingOutput(k_ESteamNetworkingSocketsDebugOutputType_Bug, text);
	}

	std::unordered_map<std::string, IProperty*> props_;
	std::mutex bml_mtx_;
	std::mutex client_mtx_;
	
	asio::io_context io_ctx_;
	std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
	//std::thread io_ctx_thread_;
	std::unique_ptr<asio::ip::udp::resolver> resolver_;

	asio::thread_pool thread_pool_;
	std::thread network_thread_;
	std::thread ping_thread_;

	const float RIGHT_MOST = 0.98f;

	bool init_ = false;
	//uint64_t id_ = 0;
	std::shared_ptr<text_sprite> ping_;
	std::shared_ptr<text_sprite> status_;

	game_state db_;
	game_objects objects_;

	CK3dObject* player_ball_ = nullptr;
	BallState local_ball_state_;
	//std::vector<CK3dObject*> template_balls_;
	//std::unordered_map<std::string, uint32_t> ball_name_to_idx_;
	CK_ID current_level_array_ = 0;

	std::atomic_bool resolving_endpoint_ = false;
	bool logged_in_ = false;
	float level_start_timestamp_ = 0.0f;

	bool notify_cheat_toggle_ = true;

	bool connecting() override {
		return client::connecting() || resolving_endpoint_;
	}

	bool connected() override {
		return client::connected() && logged_in_;
	}

	CK3dObject* get_current_ball() {
		if (current_level_array_ != 0)
			return static_cast<CK3dObject*>(static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(current_level_array_))->GetElementObject(0, 1));

		return nullptr;
	}

	static std::pair<std::string, std::string> parse_connection_string(const std::string& str) {
		size_t pos = str.find(":");
		std::string address = str.substr(0, pos);
		std::string port = (pos == -1 || (pos + 1) == str.length()) ? "26676" : str.substr(pos + 1);
		return { address, port };
	}

	void init_config() {
		GetConfig()->SetCategoryComment("Remote", "Which server to connect to?");
		IProperty* tmp_prop = GetConfig()->GetProperty("Remote", "ServerAddress");
		tmp_prop->SetComment("Remote server address with port. It could be an IP address or a domain name.");
		tmp_prop->SetDefaultString("127.0.0.1:26676");
		props_["remote_addr"] = tmp_prop;
		/*tmp_prop = GetConfig()->GetProperty("Remote", "Port");
		tmp_prop->SetComment("The port that server is running on.");
		tmp_prop->SetDefaultInteger(50000);
		props_["remote_port"] = tmp_prop;*/

		GetConfig()->SetCategoryComment("Player", "Who are you?");
		tmp_prop = GetConfig()->GetProperty("Player", "Playername");
		tmp_prop->SetComment("Your name please?");
		std::srand(std::time(nullptr));
		int random_variable = std::rand() % 1000;
		std::stringstream ss;
		ss << "Player" << std::setw(3) << std::setfill('0') << random_variable;
		tmp_prop->SetDefaultString(ss.str().c_str());
		props_["playername"] = tmp_prop;
	}

	struct KeyVector {
		char x = 0;
		char y = 0;
		char z = 0;

		bool clear() {
			return x == 0 && y == 0 && z == 0;
		}

		auto operator<=>(const KeyVector&) const = default;
		/*bool operator==(const KeyVector& that) const {
			if (this == &that)
				return true;

			return
				this->x == that.x &&
				this->y == that.y &&
				this->z == that.z;
		}*/
	};

	KeyVector last_input_;

	void poll_status_toggle() {
		
	}

	/*char ckkey_to_num(CKKEYBOARD key) {
		if (key == CKKEY_0)
			return 0;

		if (key >= CKKEY_1 && key <= CKKEY_9)
			return key - CKKEY_1 + 1;

		return -1;
	}

	CKKEYBOARD num_to_ckkey(int num) {
		if (num == 0)
			return CKKEY_0;

		if (num >= 1 && num <= 9)
			return (CKKEYBOARD)(num - 1 + CKKEY_1);

		return CKKEY_AX;
	}*/
	CKBehavior* script = nullptr;
	CKBehavior* m_dynamicPos = nullptr;
	CKBehavior* m_phyNewBall = nullptr;
	//CKContext* ctx = m_bml->GetCKContext();
	CKDataArray* m_curLevel = m_bml->GetArrayByName("CurrentLevel");
	CKDataArray* m_ingameParam = m_bml->GetArrayByName("IngameParameter");
	CK_ID init_game;
	void edit_Gameplay_Ingame(CKBehavior* script) {
		CKBehavior* init_ingame = ScriptHelper::FindFirstBB(script, "Init Ingame");
		init_game = CKOBJID(init_ingame);
		CKBehavior* ballMgr = ScriptHelper::FindFirstBB(script, "BallManager");
		CKBehavior* newBall = ScriptHelper::FindFirstBB(ballMgr, "New Ball");
		m_dynamicPos = ScriptHelper::FindNextBB(script, ballMgr, "TT Set Dynamic Position");
		m_phyNewBall = ScriptHelper::FindFirstBB(newBall, "physicalize new Ball");
	}

	//CKParameter* m_curSector = nullptr;
	CK_ID m_curSector;
	void edit_Gameplay_Events(CKBehavior* script) {
		CKBehavior* id = ScriptHelper::FindNextBB(script, script->GetInput(0));
		m_curSector = CKOBJID(id->GetOutputParameter(0)->GetDestination(0));
	}

	CK_ID reset_level_;
	CK_ID pause_level_;
	void edit_Event_handler(CKBehavior* script) {
		pause_level_ = CKOBJID(ScriptHelper::FindFirstBB(script, "Pause Level"));
		reset_level_ = CKOBJID(ScriptHelper::FindFirstBB(script, "reset Level"));
	}

	const CKKEYBOARD keys_to_check[4] = { CKKEY_0, CKKEY_1, CKKEY_2, CKKEY_3 };
	const std::vector<std::string> init_args{ "mmo", "s" };
	void poll_local_input() {
		auto* input_manager = m_bml->GetInputManager();

		// Toggle status
		if (input_manager->IsKeyPressed(CKKEY_F3)) {
			ping_->toggle();
			status_->toggle();
		}

		// Toggle nametag
		if (input_manager->IsKeyPressed(CKKEY_TAB)) {
			db_.toggle_nametag_visible();
		}

		if (input_manager->IsKeyDown(CKKEY_LCONTROL)) {
			for (int i = 0; i <= 3; ++i) {
				if (input_manager->IsKeyPressed(keys_to_check[i])) {
					std::vector<std::string> args(init_args);
					if (i == 0) {
						args.emplace_back("Go!");
					}
					else {
						args.emplace_back(std::to_string(i));
					}
					OnCommand(m_bml, args);
				}
			}
		}
		
		//if (input_manager->IsKeyPressed(CKKEY_5)) {
		//	/*m_bml->OnBallNavInactive();
		//	m_bml->OnPreResetLevel();
		//	CK3dEntity* curBall = static_cast<CK3dEntity*>(m_bml->GetArrayByName("CurrentLevel")->GetElementObject(0, 1));
		//	if (curBall) {
		//		ExecuteBB::Unphysicalize(curBall);
		//	}
		//	auto* in = static_cast<CKBehavior*>(m_bml->GetCKContext()->GetObject(init_game));
		//	in->ActivateInput(0);
		//	in->Activate();
		//	m_bml->OnPostResetLevel();
		//	m_bml->OnStartLevel();*/
		//	/*auto* beh = static_cast<CKBehavior*>(m_bml->GetCKContext()->GetObject(pause_level_));
		//	beh->ActivateInput(0);
		//	beh->Activate();*/
		//	m_bml->OnPauseLevel();
		//	m_bml->OnBallNavInactive();
		//	CKMessageManager* mm = m_bml->GetMessageManager();
		//	CKMessageType blend = mm->AddMessageType("Blend FadeIn");
		//	mm->SendMessageSingle(blend, static_cast<CKBeObject*>(m_bml->GetCKContext()->GetObjectByNameAndParentClass("Level", CKCID_BEOBJECT, nullptr)));
		//	auto* beh = static_cast<CKBehavior*>(m_bml->GetCKContext()->GetObject(reset_level_));
		//	beh->ActivateInput(0);
		//	beh->Activate();
		//}
		
		/*if (input_manager->IsKeyPressed(CKKEY_P)) {
			auto* ctx = m_bml->GetCKContext();
			CKMessageManager* mm = m_bml->GetMessageManager();
			CKMessageType ballDeact = mm->AddMessageType("BallNav deactivate");

			mm->SendMessageSingle(ballDeact, m_bml->GetGroupByName("All_Gameplay"));
			mm->SendMessageSingle(ballDeact, m_bml->GetGroupByName("All_Sound"));

			m_bml->AddTimer(2u, [this, ctx]() {
				CK3dEntity* curBall = static_cast<CK3dEntity*>(m_bml->GetArrayByName("CurrentLevel")->GetElementObject(0, 1));
				if (curBall) {
					ExecuteBB::Unphysicalize(curBall);

					CKDataArray* ph = m_bml->GetArrayByName("PH");
					for (int i = 0; i < ph->GetRowCount(); i++) {
						CKBOOL set = true;
						char name[100];
						ph->GetElementStringValue(i, 1, name);
						if (!strcmp(name, "P_Extra_Point"))
							ph->SetElementValue(i, 4, &set);
					}

					auto* sector = static_cast<CKParameter*>(ctx->GetObject(m_curSector));
					m_bml->GetArrayByName("IngameParameter")->SetElementValueFromParameter(0, 1, sector);
					m_bml->GetArrayByName("IngameParameter")->SetElementValueFromParameter(0, 2, sector);
					CKBehavior* sectorMgr = m_bml->GetScriptByName("Gameplay_SectorManager");
					ctx->GetCurrentScene()->Activate(sectorMgr, true);

					m_bml->AddTimerLoop(1u, [this, curBall, sectorMgr, ctx]() {
						if (sectorMgr->IsActive())
							return true;

						m_dynamicPos->ActivateInput(1);
						m_dynamicPos->Activate();

						m_bml->AddTimer(1u, [this, curBall, sectorMgr, ctx]() {
							VxMatrix matrix;
							m_bml->GetArrayByName("CurrentLevel")->GetElementValue(0, 3, &matrix);
							curBall->SetWorldMatrix(matrix);

							CK3dEntity* camMF = m_bml->Get3dEntityByName("Cam_MF");
							m_bml->RestoreIC(camMF, true);
							camMF->SetWorldMatrix(matrix);

							m_bml->AddTimer(1u, [this]() {
								m_dynamicPos->ActivateInput(0);
								m_dynamicPos->Activate();

								m_phyNewBall->ActivateInput(0);
								m_phyNewBall->Activate();
								m_phyNewBall->GetParent()->Activate();

								GetLogger()->Info("Sector Reset");
								});
							});

						return false;
						});
				}
				});
		}*/

		/*BYTE* states = m_bml->GetInputManager()->GetKeyboardState();

		KeyVector current_input;

		bool w = states[CKKEY_W] & KEY_PRESSED;
		bool a = states[CKKEY_A] & KEY_PRESSED;
		bool s = states[CKKEY_S] & KEY_PRESSED;
		bool d = states[CKKEY_D] & KEY_PRESSED;

		current_input.x += (w ? 1 : 0);
		current_input.z += (a ? 1 : 0);
		current_input.x += (s ? -1 : 0);
		current_input.z += (d ? -1 : 0);

		if (current_input == last_input_) {
			return;
		}

		ExecuteBB::UnsetPhysicsForce(player_ball_);
		last_input_ = current_input;

		if (current_input.clear()) {
			return;
		}

		VxVector direction(current_input.x, current_input.y, current_input.z);

		ExecuteBB::SetPhysicsForce(
			player_ball_,
			VxVector(0, 0, 0),
			player_ball_,
			direction,
			m_bml->Get3dObjectByName("Cam_OrientRef"),
			.43f);*/

		//ExecuteBB::PhysicsWakeUp(player_ball_); // Still not merged in upstream
	}

	void check_on_trafo(CK3dObject* ball) {
		if (strcmp(ball->GetName(), player_ball_->GetName()) != 0) {
			// OnTrafo
			GetLogger()->Info("OnTrafo, %s -> %s", player_ball_->GetName(), ball->GetName());
			OnTrafo(db_.get_ball_id(player_ball_->GetName()), db_.get_ball_id(ball->GetName()));
			// Update current player ball
			player_ball_ = ball;
			local_ball_state_.type = db_.get_ball_id(player_ball_->GetName());
		}
	}

	void poll_player_ball_state() {
		player_ball_->GetPosition(&local_ball_state_.position);
		player_ball_->GetQuaternion(&local_ball_state_.rotation);
	}

	void assemble_and_send_state() {
		bmmo::ball_state_msg msg{};
		assert(sizeof(msg.content) == sizeof(local_ball_state_));
		std::memcpy(&(msg.content), &local_ball_state_, sizeof(msg.content));
		send(msg, k_nSteamNetworkingSend_UnreliableNoNagle);
#ifdef DEBUG
		GetLogger()->Info("(%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f, %.2f)",
			local_ball_state_.position.x,
			local_ball_state_.position.y,
			local_ball_state_.position.z,
			local_ball_state_.rotation.x,
			local_ball_state_.rotation.y,
			local_ball_state_.rotation.z,
			local_ball_state_.rotation.w
		);
#endif // DEBUG
	}

	void cleanup(bool down = false, bool linger = true) {
		shutdown(linger);
		
		// Weird bug if join thread here. Will join at the place before next use
		// Actually since we're using std::jthread, we don't have to join threads manually
		// Welp, std::jthread does not work on some of the clients. Switching back to std::thread. QwQ
		// 
		//if (ping_thread_.joinable())
		//	ping_thread_.join();
		// 
		//if (network_thread_.joinable())
			//network_thread_.join();
		
		//thread_pool_.stop();
		db_.clear();
		objects_.destroy_all_objects();

		if (!io_ctx_.stopped())
			io_ctx_.stop();

		resolving_endpoint_ = false;
		logged_in_ = false;

		if (down) // Since the game's going down, we don't care about text shown.
			return;

		if (ping_)
			ping_->update("");

		if (status_) {
			status_->update("Disconnected");
			status_->paint(0xffff0000);
		}
	}

	static std::string pretty_percentage(float value) {
		if (value < 0)
			return "N/A";

		return std::format("{:.2f}%", value * 100.0f);
	}

	static std::string pretty_bytes(float bytes)
	{
		const char* suffixes[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB" };
		int s = 0; // which suffix to use
		while (bytes >= 1024 && s < 7)
		{
			++s;
			bytes /= 1024;
		}

		return std::format("{:.1f}{}", bytes, suffixes[s]);
	}

	static std::string pretty_status(const SteamNetConnectionRealTimeStatus_t& status) {
		std::string s;
		s.reserve(2048);
		s += std::format("Ping: {} ms\n", status.m_nPing);
		s += "ConnectionQualityLocal: " + pretty_percentage(status.m_flConnectionQualityLocal) + "\n";
		s += "ConnectionQualityRemote: " + pretty_percentage(status.m_flConnectionQualityRemote) + "\n";
		s += std::format("Tx: {:.0f}pps, ", status.m_flOutPacketsPerSec) + pretty_bytes(status.m_flOutBytesPerSec) + "/s\n";
		s += std::format("Rx: {:.0f}pps, ", status.m_flInPacketsPerSec) + pretty_bytes(status.m_flInBytesPerSec) + "/s\n";
		s += "Est. MaxBandwidth: " + pretty_bytes(status.m_nSendRateBytesPerSecond) + "/s\n";
		s += std::format("Queue time: {}us\n", status.m_usecQueueTime);
		s += std::format("\nReliable:            \nPending: {}\nUnacked: {}\n", status.m_cbPendingReliable, status.m_cbSentUnackedReliable);
		s += std::format("\nUnreliable:          \nPending: {}\n", status.m_cbPendingUnreliable);
		return s;
	}

	std::string join_strings(const std::vector<std::string>& strings, int start) const {
		std::string str = strings[start];
		start++;
		size_t length = strings.size();
		if (length > start) {
			for (size_t i = start; i < length; i++)
				str.append(" " + strings[i]);
		}
		if (str.length() > 65536) {
			throw "Error: message too long.";
			return "";
		}
		return str;
	}

	/*CKBehavior* bbSetForce = nullptr;
	static void SetForce(CKBehavior* bbSetForce, CK3dEntity* target, VxVector position, CK3dEntity* posRef, VxVector direction, CK3dEntity* directionRef, float force) {
		using namespace ExecuteBB;
		using namespace ScriptHelper;
		SetParamObject(bbSetForce->GetTargetParameter()->GetDirectSource(), target);
		SetParamValue(bbSetForce->GetInputParameter(0)->GetDirectSource(), position);
		SetParamObject(bbSetForce->GetInputParameter(1)->GetDirectSource(), posRef);
		SetParamValue(bbSetForce->GetInputParameter(2)->GetDirectSource(), direction);
		SetParamObject(bbSetForce->GetInputParameter(3)->GetDirectSource(), directionRef);
		SetParamValue(bbSetForce->GetInputParameter(4)->GetDirectSource(), force);
		bbSetForce->ActivateInput(0);
		bbSetForce->Execute(0);
	}

	static void UnsetPhysicsForce(CKBehavior* bbSetForce, CK3dEntity* target) {
		using namespace ExecuteBB;
		using namespace ScriptHelper;
		SetParamObject(bbSetForce->GetTargetParameter()->GetDirectSource(), target);
		bbSetForce->ActivateInput(1);
		bbSetForce->Execute(0);
	}*/
};