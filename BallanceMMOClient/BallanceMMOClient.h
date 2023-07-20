#pragma once

#include <BML/BMLAll.h>
#include "CommandMMO.h"
#include "text_sprite.h"
#include "label_sprite.h"
#include "exported_client.h"
#include "game_state.h"
#include "game_objects.h"
#include "local_state_handler_impl.h"
#include "dumpfile.h"
#include <map>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <format>
#include <ranges>
#include <asio.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/circular_buffer.hpp>
#include <openssl/md5.h>
// #include <openssl/sha.h>
#include <fstream>
#include <io.h>
#include <fcntl.h>
#include <ShlObj.h>

#define PICOJSON_USE_INT64
#include <picojson/picojson.h>

// #include <filesystem>

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class BallanceMMOClient : public IMod, public bmmo::exported::client {
public:
	BallanceMMOClient(IBML* bml): 
		IMod(bml),
		objects_(bml, db_)
		//client_([this](ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) { LoggingOutput(eType, pszMsg); },
		//	[this](SteamNetConnectionStatusChangedCallback_t* pInfo) { OnConnectionStatusChanged(pInfo); })
	{
		DeclareDumpFile(std::bind(&BallanceMMOClient::on_fatal_error, this));
		this_instance_ = this;

		LOGFONT font_struct{};
		SystemParametersInfo(SPI_GETICONTITLELOGFONT, sizeof(font_struct), &font_struct, 0);
		strcpy(system_font_, font_struct.lfFaceName);
	}

	bmmo::version_t version;
	const std::string version_string = version.to_string();
	virtual CKSTRING GetID() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetVersion() override { return version_string.c_str(); }
	virtual CKSTRING GetName() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetAuthor() override { return "Swung0x48 & BallanceBug"; }
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

	// virtual functions from bmmo::exported::client
	std::string get_client_name() override { return db_.get_nickname(); }
	bool is_spectator() override { return spectator_mode_; }
	bmmo::named_map get_current_map() override { return current_map_; }

private:
	void OnLoad() override;
	void OnPostStartMenu() override;
	void OnExitGame() override;
	//void OnUnload() override;
	void OnProcess() override;
	void OnStartLevel() override;
	void OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj) override;
	void OnPostCheckpointReached() override;
	void OnPostExitLevel() override;
	void OnCounterActive() override;
	void OnLevelFinish() override;
	void OnLoadScript(CKSTRING filename, CKBehavior* script) override;
	void OnCheatEnabled(bool enable) override;
	void OnModifyConfig(CKSTRING category, CKSTRING key, IProperty* prop) override;
	// Custom
	void OnCommand(IBML* bml, const std::vector<std::string>& args);
	const std::vector<std::string> OnTabComplete(IBML* bml, const std::vector<std::string>& args);
	void OnTrafo(int from, int to);
	void OnPeerTrafo(uint64_t id, int from, int to);

	// Callbacks from client
	static void LoggingOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg);
	void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) override;
	void on_message(ISteamNetworkingMessage* network_msg) override;

	void on_fatal_error() {
		if (!connected())
			return;
		bmmo::simple_action_msg msg{};
		msg.content.action = bmmo::action_type::FatalError;
		send(msg, k_nSteamNetworkingSend_Reliable);
	};

	inline void on_sector_changed();

	bool show_console();
	bool hide_console();
	void show_player_list();

	void connect_to_server(std::string address = "");
	void disconnect_from_server();
	void reconnect(int delay);

	int reconnection_count_ = 0;

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
	std::thread console_thread_;
	std::thread player_list_thread_;

	std::atomic_bool console_running_ = false;
	std::atomic_bool player_list_visible_ = false;

	const float RIGHT_MOST = 0.98f;
	CKDWORD player_list_color_ = 0xFFFFFFFF;

	bool init_ = false;
	//uint64_t id_ = 0;
	std::shared_ptr<text_sprite> ping_;
	std::shared_ptr<text_sprite> status_;
	std::shared_ptr<text_sprite> spectator_label_, permanent_notification_;

	game_state db_;
	game_objects objects_;

	CK3dObject* player_ball_ = nullptr;
	//std::vector<CK3dObject*> template_balls_;
	//std::unordered_map<std::string, uint32_t> ball_name_to_idx_;
	CK_ID current_level_array_ = 0;
	CK_ID ingame_parameter_array_ = 0;
	bmmo::named_map current_map_{};
	int32_t current_sector_ = 0;
	int32_t current_sector_timestamp_ = 0;
	std::unordered_map<std::string, std::string> map_names_;
	uint8_t balls_nmo_md5_[16]{};

	std::unique_ptr<local_state_handler_base> local_state_handler_;
	bool spectator_mode_ = false;
	std::string server_addr_;
	std::atomic_bool resolving_endpoint_ = false;
	bool logged_in_ = false;
	std::unordered_map<std::string, float> level_start_timestamp_;
	SteamNetworkingMicroseconds next_update_timestamp_ = 0, last_dnf_hotkey_timestamp_ = 0,
		dnf_cooldown_end_timestamp_ = 0;

	bool notify_cheat_toggle_ = true;
	bool reset_rank_ = false, reset_timer_ = true;
	bool countdown_restart_ = false;

	boost::uuids::uuid uuid_{};
	int64_t last_name_change_time_{};
	bool name_changed_ = false, bypass_name_check_ = false;

	char system_font_[32]{};
	const std::wstring LOCAL_APPDATA_PATH = [] { // local appdata
		std::wstring path_str = L".";
		wchar_t* path_pchar{};
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path_pchar))) {
			path_str = path_pchar;
			CoTaskMemFree(path_pchar);
		}
		return path_str;
	}();
	const std::wstring LEGACY_UUID_FILE_PATH = LOCAL_APPDATA_PATH + L"\\BallanceMMOClient_UUID.cfg";
	const std::wstring EXTERNAL_CONFIG_PATH = LOCAL_APPDATA_PATH + L"\\BallanceMMOClient_external.json";

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

	bool update_current_sector() {
		int sector = 0;
		if (ingame_parameter_array_ != 0) {
			static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(ingame_parameter_array_))->GetElementValue(0, 1, &sector);
			if (sector == current_sector_) return false;
		} else if (current_sector_ == 0) return false;
		current_sector_ = sector;
		current_sector_timestamp_ = int32_t((SteamNetworkingUtils()->GetLocalTimestamp() - db_.get_init_timestamp()) / 1024);
		return true;
	}

	void parse_and_set_player_list_color(IProperty* prop) {
		CKDWORD color = 0xFFFFE3A1;
		try {
			color = (CKDWORD) std::stoul(prop->GetString(), nullptr, 16);
		} catch (const std::exception& e) {
			GetLogger()->Warn("Error parsing the color code: %s. Resetting to %06X.", e.what(), color & 0x00FFFFFF);
			prop->SetString(std::format("{:06X}", color & 0x00FFFFFF).data());
		}
		if (player_list_color_ == color) return;
		player_list_color_ = color | 0xFF000000;
		prop->SetString(std::format("{:06X}", color & 0x00FFFFFF).data());
		if (player_list_visible_) {
			player_list_visible_ = false;
			show_player_list();
		}
		if (permanent_notification_) permanent_notification_->paint(player_list_color_);
	}

	void init_config() {
		migrate_config();
		GetConfig()->SetCategoryComment("Remote", "Which server to connect to?");
		IProperty* tmp_prop = GetConfig()->GetProperty("Remote", "ServerAddress");
		tmp_prop->SetComment("Remote server address with port (optional; default 26676). It could be an IPv4 address or a domain name.");
		tmp_prop->SetDefaultString("127.0.0.1");
		props_["remote_addr"] = tmp_prop;
		/*tmp_prop = GetConfig()->GetProperty("Remote", "Port");
		tmp_prop->SetComment("The port that server is running on.");
		tmp_prop->SetDefaultInteger(50000);
		props_["remote_port"] = tmp_prop;*/
		tmp_prop = GetConfig()->GetProperty("Remote", "SpectatorMode");
		tmp_prop->SetComment("Whether to connect to the server as a spectator. Spectators are invisible to other players.");
		tmp_prop->SetDefaultBoolean(false);
		props_["spectator"] = tmp_prop;

		GetConfig()->SetCategoryComment("Player", "Player name. Can only be changed once every 24 hours (counting down after joining a server).");
		tmp_prop = GetConfig()->GetProperty("Player", "Playername");
		tmp_prop->SetComment("Your name please?");
		tmp_prop->SetDefaultString(bmmo::name_validator::get_random_nickname().c_str());
		props_["playername"] = tmp_prop;
		// Validation of player names fails at this stage of initialization
		// so we had to put it at the time of post startmenu events.
		load_external_config();
		GetConfig()->SetCategoryComment("Gameplay", "Settings for your actual gameplay experience in multiplayer.");
		tmp_prop = GetConfig()->GetProperty("Gameplay", "Extrapolation");
		tmp_prop->SetComment("Apply quadratic extrapolation to make movement of balls look smoother at a slight cost of accuracy.");
		tmp_prop->SetBoolean(true); // force extrapolation for now
		objects_.toggle_extrapolation(tmp_prop->GetBoolean());
		props_["extrapolation"] = tmp_prop;
		tmp_prop = GetConfig()->GetProperty("Gameplay", "PlayerListColor");
		tmp_prop->SetComment("Text color of the player list (press Ctrl+Tab to toggle visibility) in hexadecimal RGB format. Default: FFE3A1");
		tmp_prop->SetDefaultString("FFE3A1");
		parse_and_set_player_list_color(tmp_prop);
		props_["player_list_color"] = tmp_prop;
		tmp_prop = GetConfig()->GetProperty("Gameplay", "DynamicOpacity");
		tmp_prop->SetComment("Whether to dynamically adjust opacities of other spirit balls based on their distances to the current camera.");
		tmp_prop->SetDefaultBoolean(true);
		objects_.toggle_dynamic_opacity(tmp_prop->GetBoolean());
		props_["dynamic_opacity"] = tmp_prop;
	}

	// ver <= 3.4.5-alpha6: no external config
	// 3.4.5-alpha6 < ver < 3.4.8-beta12: external plain text uuid config
	// ver >= 3.4.8-alpha12: external json config
	void migrate_config() {
		constexpr const char* const config_path = "..\\ModLoader\\Config\\BallanceMMOClient.cfg";
		std::ifstream config(config_path);
		if (!config.is_open())
			return;
		std::string temp_str;
		while (config >> temp_str) {
			if (temp_str == GetID())
				break;
		}
		config >> temp_str >> temp_str;
		auto version = bmmo::version_t::from_string(temp_str);
		if (version >= bmmo::version_t{3, 4, 8, bmmo::Beta, 12})
			return;

		GetLogger()->Info("Migrating config data ...");
		if (version <= bmmo::version_t{3, 4, 5, bmmo::Alpha, 6}) {
			while (config >> temp_str) {
				if (temp_str == "UUID")
					break;
			}
			if (config.eof())
				temp_str = boost::uuids::to_string(boost::uuids::random_generator()());
			else
				config >> temp_str;
		}
		else {
		  {
				std::ifstream uuid_config(LEGACY_UUID_FILE_PATH);
				if (!uuid_config.is_open())
					return;
				uuid_config >> temp_str;
			}
			DeleteFileW(LEGACY_UUID_FILE_PATH.c_str());
		}

		save_external_config(temp_str);
	}

	void load_external_config() {
		try {
			std::ifstream ifile(EXTERNAL_CONFIG_PATH);
			picojson::value external_config_v;
			ifile >> external_config_v;
			auto& external_config = external_config_v.get<picojson::object>();
			uuid_ = boost::lexical_cast<boost::uuids::uuid>(external_config["uuid"].get<std::string>());
			last_name_change_time_ = external_config["last_name_change"].get<int64_t>();
		}
		catch (...) {
			GetLogger()->Warn("Invalid UUID. A new UUID has been generated.");
			uuid_ = boost::uuids::random_generator()();
			save_external_config();
		}
	}

	void save_external_config(std::string uuid = {}) {
		if (uuid.empty())
			uuid = boost::uuids::to_string(uuid_);
		picojson::object external_config{
			{"uuid", picojson::value{uuid}},
			{"last_name_change", picojson::value{last_name_change_time_}},
		};
		std::ofstream ofile(EXTERNAL_CONFIG_PATH);
		ofile << picojson::value{external_config}.serialize(true);
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
	CK_ID init_game{};
	void edit_Gameplay_Ingame(CKBehavior* script) {
		CKBehavior* init_ingame = ScriptHelper::FindFirstBB(script, "Init Ingame");
		init_game = CKOBJID(init_ingame);
		CKBehavior* ballMgr = ScriptHelper::FindFirstBB(script, "BallManager");
		CKBehavior* newBall = ScriptHelper::FindFirstBB(ballMgr, "New Ball");
		m_dynamicPos = ScriptHelper::FindNextBB(script, ballMgr, "TT Set Dynamic Position");
		m_phyNewBall = ScriptHelper::FindFirstBB(newBall, "physicalize new Ball");
	}

	//CKParameter* m_curSector = nullptr;
	CK_ID m_curSector{};
	CK_ID esc_event_{};
	void edit_Gameplay_Events(CKBehavior* script) {
		CKBehavior* id = ScriptHelper::FindNextBB(script, script->GetInput(0));
		m_curSector = CKOBJID(id->GetOutputParameter(0)->GetDestination(0));

		auto* esc = ScriptHelper::FindFirstBB(script, "Key Event");
		esc_event_ = CKOBJID(esc->GetOutput(0));
	}

	//CK_ID ;
	void edit_Gameplay_Energy(CKBehavior* script) {
		//ScriptHelper::FindNextBB(script, script->GetInput(0));
	}

	CK_ID tutorial_exit_event_{};
	void edit_Gameplay_Tutorial(CKBehavior* script) {
		auto* tutorial_logic =
			ScriptHelper::FindFirstBB(ScriptHelper::FindFirstBB(script,
			"Kapitel Aktion"), "Tut continue/exit");
		auto* tutorial_exit =
			ScriptHelper::FindPreviousBB(tutorial_logic,
				ScriptHelper::FindFirstBB(tutorial_logic, "Set Physics Globals")->GetInput(0));
		tutorial_exit_event_ = CKOBJID(tutorial_exit->GetOutput(0));
	}

	CK_ID reset_level_{};
	CK_ID pause_level_{};
	void edit_Event_handler(CKBehavior* script) {
		pause_level_ = CKOBJID(ScriptHelper::FindFirstBB(script, "Pause Level"));
		reset_level_ = CKOBJID(ScriptHelper::FindFirstBB(script, "reset Level"));
	}

	CK_ID restart_level_{};
	CK_ID menu_pause_{};
	CK_ID exit_{};
	void edit_Menu_Pause(CKBehavior* script) {
		restart_level_ = CKOBJID(ScriptHelper::FindFirstBB(script, "Restart Level"));
		menu_pause_ = CKOBJID(script);
		exit_ = CKOBJID(ScriptHelper::FindFirstBB(script, "Exit"));
	}

	std::atomic_bool own_ball_visible_ = false;
	std::mutex ball_toggle_mutex_;
	void toggle_own_spirit_ball(bool visible, bool notify = false) {
		std::lock_guard lk(ball_toggle_mutex_);
		if (own_ball_visible_ == visible)
			return;
		GetLogger()->Info("Toggling visibility of own ball to %s", visible ? "on" : "off");
		if (visible) {
			objects_.init_player(db_.get_client_id(), db_.get_nickname(), m_bml->IsCheatEnabled());
			db_.create(db_.get_client_id(), db_.get_nickname(), m_bml->IsCheatEnabled());
			db_.update(db_.get_client_id(), TimedBallState(local_state_handler_->get_local_state()));
		}
		else {
			db_.remove(db_.get_client_id());
			objects_.remove(db_.get_client_id());
		}
		own_ball_visible_ = visible;
		if (notify)
			SendIngameMessage(std::string("Set own spirit ball to ") + (visible ? "visible" : "hidden"));
	}

	InputHook* input_manager_ = nullptr;
	static constexpr CKKEYBOARD KEYS_TO_CHECK[] = { CKKEY_0, CKKEY_1, CKKEY_2, CKKEY_3, CKKEY_4, CKKEY_5 };
	// const std::vector<std::string> init_args{ "mmo", "s" };
	void poll_local_input() {
		// Toggle status
		if (input_manager_->IsKeyDown(CKKEY_F3)) {
			if (input_manager_->IsKeyPressed(CKKEY_A)) {
				if (!connected() || !m_bml->IsIngame())
					return;
				objects_.reload();
				SendIngameMessage("Reload completed.");
			} else if (input_manager_->IsKeyPressed(CKKEY_H)) {
				if (!connected())
					return;
			} else if (input_manager_->IsKeyPressed(CKKEY_F3)) {
				ping_->toggle();
				status_->toggle();
			}
		}

		if (input_manager_->IsKeyDown(CKKEY_LCONTROL) && connected()) {
		  if (m_bml->IsIngame()) {
				for (int i = 0; i < sizeof(KEYS_TO_CHECK) / sizeof(CKKEYBOARD); ++i) {
					if (input_manager_->IsKeyPressed(KEYS_TO_CHECK[i])) {
						// std::vector<std::string> args(init_args);
						// OnCommand(m_bml, args);
						send_countdown_message(static_cast<bmmo::countdown_type>(i));
					}
				}
				if (input_manager_->IsKeyPressed(CKKEY_GRAVE)) {
					// toggle own ball
					toggle_own_spirit_ball(!own_ball_visible_, true);
				}
				if (input_manager_->IsKeyDown(CKKEY_LSHIFT) && input_manager_->IsKeyPressed(CKKEY_UP)) {
					CK3dEntity* camMF = m_bml->Get3dEntityByName("Cam_MF");
					VxVector orient[3] = { {0, 0, -1}, {0, 1, 0}, {-1, 0, 0} };
					VxVector pos;
					camMF->GetPosition(&pos);
					m_bml->RestoreIC(camMF, true);
					camMF->SetOrientation(orient[0], orient[1], orient + 2);
					camMF->SetPosition(pos);
					m_dynamicPos->ActivateInput(0);
					m_dynamicPos->Activate();
				}
			}
			if (input_manager_->IsKeyPressed(CKKEY_D)) {
				if (current_map_.level == 0 || spectator_mode_)
					return;
				auto timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
				if (timestamp < dnf_cooldown_end_timestamp_)
					return;
				if (timestamp - last_dnf_hotkey_timestamp_ <= 3000000) {
					bmmo::did_not_finish_msg msg{};
					m_bml->GetArrayByName("IngameParameter")->GetElementValue(0, 1, &msg.content.sector);
					msg.content.map = current_map_;
					msg.content.cheated = m_bml->IsCheatEnabled();
					send(msg, k_nSteamNetworkingSend_Reliable);
					last_dnf_hotkey_timestamp_ = 0;
					dnf_cooldown_end_timestamp_ = timestamp + 6000000;
				}
				else {
					last_dnf_hotkey_timestamp_ = timestamp;
					SendIngameMessage("Note: please press Ctrl+D again in 3 seconds to send the DNF message.");
				}
			}
			if (input_manager_->IsKeyPressed(CKKEY_TAB)) {
				if (player_list_visible_)
					player_list_visible_ = false;
				else
					show_player_list();
				return;
			}
		}

		// Toggle nametag
		if (input_manager_->IsKeyPressed(CKKEY_TAB)) {
			db_.toggle_nametag_visible();
		}

#ifdef DEBUG
		if (input_manager->IsKeyPressed(CKKEY_5)) {
			restart_current_level();
		}

		if (input_manager->IsKeyPressed(CKKEY_6)) {
			m_bml->RestoreIC(static_cast<CKBeObject*>(m_bml->GetCKContext()->GetObjectByName("Menu_Pause_ShowHide")));
		}
#endif
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
			local_state_handler_->set_ball_type(db_.get_ball_id(player_ball_->GetName()));
		}
	}

	void cleanup(bool down = false, bool linger = true) {
		if (player_list_visible_) {
			player_list_visible_ = false;
			asio::post(thread_pool_, [this] {
				if (player_list_thread_.joinable()) player_list_thread_.join();
			});
		}
		if (down) {
			console_running_ = false;
			asio::post(thread_pool_, [this] {
				FreeConsole();
				if (console_thread_.joinable())
					console_thread_.join();
			});
		}

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
		toggle_own_spirit_ball(false);
		map_names_.clear();
		db_.clear();
		objects_.destroy_all_objects();
		local_state_handler_.reset();

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

		spectator_label_.reset();
		permanent_notification_.reset();
		db_.set_client_id(k_HSteamNetConnection_Invalid + ((rand() << 16) | rand())); // invalid id indicates server
	}

	void restart_current_level() {
		/*m_bml->OnBallNavInactive();
		m_bml->OnPreResetLevel();
		CK3dEntity* curBall = static_cast<CK3dEntity*>(m_bml->GetArrayByName("CurrentLevel")->GetElementObject(0, 1));
		if (curBall) {
			ExecuteBB::Unphysicalize(curBall);
		}
		auto* in = static_cast<CKBehavior*>(m_bml->GetCKContext()->GetObject(init_game));
		in->ActivateInput(0);
		in->Activate();
		m_bml->OnPostResetLevel();
		m_bml->OnStartLevel();*/
		/*auto* pause = static_cast<CKBehavior*>(m_bml->GetCKContext()->GetObject(pause_level_));
		pause->ActivateInput(0);
		pause->Activate();*/
		//m_bml->OnPauseLevel();
		//m_bml->OnBallNavInactive();
		
		//INPUT ip;
		//ip.type = INPUT_KEYBOARD;
		//ip.ki.wScan = 0; // hardware scan code for key
		//ip.ki.time = 0;
		//ip.ki.dwExtraInfo = 0;

		//ip.ki.wVk = VK_ESCAPE;
		//ip.ki.dwFlags = 0; // 0 for key press
		//SendInput(1, &ip, sizeof(INPUT));

		//ip.ki.dwFlags = KEYEVENTF_KEYUP; // KEYEVENTF_KEYUP for key release
		//SendInput(1, &ip, sizeof(INPUT));
		
		auto* esc = static_cast<CKBehaviorIO*>(m_bml->GetCKContext()->GetObject(esc_event_));
		esc->Activate();

		m_bml->AddTimer(3u, [this]() {
			CKMessageManager* mm = m_bml->GetMessageManager();

			CKMessageType reset_level_msg = mm->AddMessageType("Reset Level");
			mm->SendMessageSingle(reset_level_msg, static_cast<CKBeObject*>(m_bml->GetCKContext()->GetObjectByNameAndParentClass("Level", CKCID_BEOBJECT, nullptr)));
			mm->SendMessageSingle(reset_level_msg, static_cast<CKBeObject*>(m_bml->GetCKContext()->GetObjectByNameAndParentClass("All_Balls", CKCID_BEOBJECT, nullptr)));

			auto* beh = static_cast<CKBehavior*>(m_bml->GetCKContext()->GetObject(restart_level_));
			auto* output = beh->GetOutput(0);
			output->Activate();
		});
		
		
		//auto* beh = static_cast<CKBehavior*>(m_bml->GetCKContext()->GetObject(menu_pause_));
		//beh->Activate(FALSE);
		
		//beh = static_cast<CKBehavior*>(m_bml->GetCKContext()->GetObject(exit_));
		//beh->ActivateInput(0);
		//beh->Activate();
	}

	void validate_nickname(IProperty* name_prop) {
		std::string name = name_prop->GetString();
		if (!bmmo::name_validator::is_valid(name)) {
			std::string valid_name = bmmo::name_validator::get_valid_nickname(name);
			SendIngameMessage(std::format(
				"Invalid player name \"{}\", replaced with \"{}\".",
				name, valid_name).c_str());
			bypass_name_check_ = true;
			name_prop->SetString(valid_name.c_str());
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

	static inline const std::string MICRO_SIGN = bmmo::string_utils::ConvertWideToANSI(bmmo::string_utils::ConvertUtf8ToWide("µ"));
	static std::string pretty_status(const SteamNetConnectionRealTimeStatus_t& status) {
		std::string s;
		s.reserve(512);
		s += std::format("Ping: {} ms\n", status.m_nPing);
		s += "ConnectionQualityLocal: " + pretty_percentage(status.m_flConnectionQualityLocal) + "\n";
		s += "ConnectionQualityRemote: " + pretty_percentage(status.m_flConnectionQualityRemote) + "\n";
		s += std::format("Tx: {:.3g}pps, ", status.m_flOutPacketsPerSec) + pretty_bytes(status.m_flOutBytesPerSec) + "/s\n";
		s += std::format("Rx: {:.3g}pps, ", status.m_flInPacketsPerSec) + pretty_bytes(status.m_flInBytesPerSec) + "/s\n";
		s += "Est. MaxBandwidth: " + pretty_bytes((float)status.m_nSendRateBytesPerSecond) + "/s\n";
		s += std::format("Queue time: {}{}s\n", status.m_usecQueueTime, MICRO_SIGN);
		s += std::format("\nReliable:            \nPending: {}\nUnacked: {}\n", status.m_cbPendingReliable, status.m_cbSentUnackedReliable);
		s += std::format("\nUnreliable:          \nPending: {}\n", status.m_cbPendingUnreliable);
		return s;
	}

	void send_countdown_message(bmmo::countdown_type type) {
		bmmo::countdown_msg msg{};
		msg.content.type = type;
		msg.content.map = current_map_;
		msg.content.force_restart = reset_rank_;
		reset_rank_ = false;
		send(msg, k_nSteamNetworkingSend_Reliable);
	}

	void send_current_map_name() {
		bmmo::map_names_msg msg{};
		msg.maps[current_map_.get_hash_bytes_string()] = current_map_.name;
		msg.serialize();
		send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
	}

	void send_current_map(bmmo::current_map_state::state_type type = bmmo::current_map_state::EnteringMap) {
		bmmo::current_map_msg msg{};
		msg.content.map = current_map_;
		msg.content.type = type;
		msg.content.sector = current_sector_;
		send(msg, k_nSteamNetworkingSend_Reliable);
	}

	void send_current_sector() {
		send(bmmo::current_sector_msg{.content = {.sector = current_sector_}}, k_nSteamNetworkingSend_Reliable);
	}

	std::string get_username(HSteamNetConnection client_id) {
		if (client_id == k_HSteamNetConnection_Invalid)
			return {"[Server]"};
		auto state = db_.get(client_id);
		assert(state.has_value() || (db_.get_client_id() == msg.player_id));
		return state.has_value() ? state->name : db_.get_nickname();
	}

	void md5_from_file(const std::string& path, uint8_t* result) {
		std::ifstream file(path, std::ifstream::binary);
		if (!file.is_open())
			return;
		MD5_CTX md5Context;
		MD5_Init(&md5Context);
		constexpr static size_t SIZE = 1024 * 16;
		auto buf = std::make_unique_for_overwrite<char[]>(SIZE);
		while (file.good()) {
			file.read(buf.get(), SIZE);
			MD5_Update(&md5Context, buf.get(), (size_t)file.gcount());
		}
		MD5_Final(result, &md5Context);
	}

	boost::circular_buffer<std::string> previous_msg_ = decltype(previous_msg_)(8);
	
	// Windows 7 does not have GetDpiForSystem
	typedef UINT (WINAPI* GetDpiForSystemPtr) (void);
	GetDpiForSystemPtr const get_system_dpi = [] {
		auto hMod = GetModuleHandleW(L"user32.dll");
		if (hMod) {
			return (GetDpiForSystemPtr)GetProcAddress(hMod, "GetDpiForSystem");
		}
		return (GetDpiForSystemPtr)nullptr;
	}();

	// input: desired font size on BallanceBug's screen
	// window size: 1024x768; dpi: 119
	int get_display_font_size(float size) {
		return (int)std::round(m_bml->GetRenderContext()->GetHeight() / (768.0f / 119) * size / ((get_system_dpi == nullptr) ? 96 : get_system_dpi()));
	}

	inline void flash_window() { FlashWindow((HWND)m_bml->GetCKContext()->GetMainWindow(), false); }

	void SendIngameMessage(const std::string& msg) {
		SendIngameMessage(msg.c_str());
	}

	void SendIngameMessage(const char* msg) {
		previous_msg_.push_back(msg);
		if (console_running_) {
			Printf(msg);
		}
		m_bml->SendIngameMessage(msg);
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
