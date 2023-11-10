#pragma once

#include "bml_includes.h"
#include "CommandMMO.h"
#include "text_sprite.h"
#include "label_sprite.h"
#include "exported_client.h"
#include "game_state.h"
#include "game_objects.h"
#include "local_state_handler_impl.h"
#include "dumpfile.h"
#include "log_manager.h"
#include "utils.h"
#include "server_list.h"
#include "config_manager.h"
#include "console_window.h"
#include <map>
#include <unordered_map>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <format>
#include <ranges>
#include <filesystem>
#include <asio.hpp>
#include <boost/regex.hpp>
// #include <openssl/sha.h>
#include <fstream>
// #include <filesystem>

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class BallanceMMOClient : public IMod, public bmmo::exported::client {
public:
	BallanceMMOClient(IBML* bml):
		IMod(bml),
		objects_(bml, db_),
		log_manager_(GetLogger(), [this](std::string msg, int ansi_color) { SendIngameMessage(msg, ansi_color); }),
		utils_(bml),
		config_manager_(&log_manager_, [this] { return GetConfig(); }),
		server_list_(bml, &log_manager_, [this](std::string addr, std::string name) { connect_to_server(addr, name); }),
		console_window_(bml, &log_manager_, [this](IBML* bml, const std::vector<std::string>& args) { OnCommand(bml, args); })
		//client_([this](ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) { LoggingOutput(eType, pszMsg); },
		//	[this](SteamNetConnectionStatusChangedCallback_t* pInfo) { OnConnectionStatusChanged(pInfo); })
	{
		DeclareDumpFile(std::bind(&BallanceMMOClient::on_fatal_error, this, std::placeholders::_1));
		this_instance_ = this;
	}

	const std::string version_string = bmmo::current_version.to_string();
	virtual BMMO_CKSTRING GetID() override { return "BallanceMMOClient"; }
	virtual BMMO_CKSTRING GetVersion() override { return version_string.c_str(); }
	virtual BMMO_CKSTRING GetName() override { return "BallanceMMOClient"; }
	virtual BMMO_CKSTRING GetAuthor() override { return "Swung0x48 & BallanceBug"; }
	virtual BMMO_CKSTRING GetDescription() override { return "The client to connect your game to the universe."; }
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

	static inline auto get_instance() { return static_cast<BallanceMMOClient*>(role::this_instance_); } // public

	HWINEVENTHOOK move_size_hook_{};

	void enter_size_move();
	void exit_size_move();

	// virtual functions from bmmo::exported::client
	std::pair<HSteamNetConnection, std::string> get_own_id() override {
		return { db_.get_client_id(), get_display_nickname() };
	}
	bool is_spectator() override { return spectator_mode_; }
	bmmo::named_map get_current_map() override { return current_map_; }
	std::unordered_map<HSteamNetConnection, std::string> get_client_list() override {
		decltype(get_client_list()) list;
		db_.for_each([&](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
			if (pair.first == db_.get_client_id() || bmmo::name_validator::is_spectator(pair.second.name))
				return true;
			list.emplace(pair.first, pair.second.name);
			return true;
		});
		return list;
	}
	void register_login_callback(IMod* mod, std::function<void()> callback) override {
		std::lock_guard lk(client_mtx_);
		login_callbacks_.emplace(mod, callback);
	};
	void register_logout_callback(IMod* mod, std::function<void()> callback) override {
		std::lock_guard lk(client_mtx_);
		logout_callbacks_.emplace(mod, callback);
	};

private:
	void OnLoad() override;
	void OnPostStartMenu() override;
	void OnExitGame() override;
	//void OnUnload() override;
	void OnProcess() override;
	void OnStartLevel() override;
	void OnLoadObject(BMMO_CKSTRING filename, BOOL isMap, BMMO_CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj) override;
	void OnPostCheckpointReached() override;
	void OnPostExitLevel() override;
	void OnCounterActive() override;
	void OnPauseLevel() override;
	void OnBallOff() override;
	void OnCamNavActive() override;
	void OnPreLifeUp() override;
	void OnLevelFinish() override;
	void OnLoadScript(BMMO_CKSTRING filename, CKBehavior* script) override;
	void OnCheatEnabled(bool enable) override;
	void OnModifyConfig(BMMO_CKSTRING category, BMMO_CKSTRING key, IProperty* prop) override;
	// Custom
	void OnCommand(IBML* bml, const std::vector<std::string>& args);
	void OnAsyncCommand(IBML* bml, const std::vector<std::string>& args);
	const std::vector<std::string> OnTabComplete(IBML* bml, const std::vector<std::string>& args);
	void OnTrafo(int from, int to);
	void OnPeerTrafo(uint64_t id, int from, int to);

	// Callbacks from client
	static void LoggingOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg);
	void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) override;
	void on_message(ISteamNetworkingMessage* network_msg) override;

	void on_fatal_error(std::string& extra_text);

	inline void on_sector_changed();

	void show_player_list();
	inline void update_player_list(text_sprite& player_list, int& last_player_count, int& last_font_size);

	void connect_to_server(std::string address, std::string name = "");
	void disconnect_from_server();
	// 3 attempts: delay, delay * scale, delay * scale ^ 2
	void reconnect(int delay, float scale = 1.0f);

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

	std::mutex bml_mtx_;
	std::mutex client_mtx_;
	std::condition_variable client_cv_;

	asio::io_context io_ctx_;
	std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
	//std::thread io_ctx_thread_;
	std::unique_ptr<asio::ip::udp::resolver> resolver_;

	asio::thread_pool thread_pool_;
	std::thread network_thread_;
	std::thread ping_thread_;
	std::thread player_list_thread_;

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

	log_manager log_manager_;
	utils utils_;
	config_manager config_manager_;
	server_list server_list_;
	console_window console_window_;
	bmmo::console console_;
	void init_commands();

	CK3dObject* player_ball_ = nullptr;
	//std::vector<CK3dObject*> template_balls_;
	//std::unordered_map<std::string, uint32_t> ball_name_to_idx_;
	CK_ID current_level_array_ = 0;
	CK_ID ingame_parameter_array_ = 0;
	CK_ID energy_array_ = 0;
	CK_ID all_gameplay_beh_ = 0;
	bmmo::named_map current_map_{};
	bmmo::map last_countdown_map_{};
	bmmo::level_mode current_level_mode_ = bmmo::level_mode::Speedrun, countdown_mode_{};
	float counter_start_timestamp_ = 0;
	int32_t current_sector_ = 0;
	int64_t current_sector_timestamp_ = 0;
	std::unordered_map<std::string, std::string> map_names_;
	uint8_t balls_nmo_md5_[16]{};

	struct map_data {
		float level_start_timestamp{};
		// pair <sector, earliest timestamp of reaching the sector>
		std::map<int, int64_t> sector_timestamps{};
		bmmo::ranking_entry::player_rankings rankings{};
	};
	std::unordered_map<std::string, map_data> maps_;

	int32_t initial_points_{}, initial_lives_{};
	float point_decrease_interval_{};

	float last_move_size_time_{}, move_size_time_length_{};

	bool ball_off_ = false, extra_life_received_ = false, level_finished_ = false;
	int compensation_lives_ = 0;
	std::unique_ptr<label_sprite> compensation_lives_label_;
	void update_compensation_lives_label();

	std::unique_ptr<local_state_handler_base> local_state_handler_;
	bool spectator_mode_ = false;
	std::string server_addr_, server_name_;
	std::atomic_bool resolving_endpoint_ = false;
	bool logged_in_ = false;
	SteamNetworkingMicroseconds next_update_timestamp_ = 0,
		last_dnf_hotkey_timestamp_ = 0, dnf_cooldown_end_timestamp_ = 0;

	bool notify_cheat_toggle_ = true;
	bool reset_rank_ = false, reset_timer_ = true;
	bool countdown_restart_ = false, did_not_finish_ = false;

	std::unordered_map<IMod*, std::function<void()>> login_callbacks_, logout_callbacks_;

	std::atomic_bool sound_enabled_ = true;
	CKWaveSound* sound_countdown_{}, * sound_go_{},
		* sound_level_finish_{}, * sound_level_finish_cheat_{}, * sound_dnf_{},
		* sound_notification_{}, * sound_bubble_{}, * sound_knock_{};
	void play_beep(uint32_t frequency, uint32_t duration) {
		if (!sound_enabled_)
			return;
		Beep(frequency, duration);
	};
	void play_wave_sound(CKWaveSound* sound, bool forced = false) {
		if (!sound_enabled_ && !forced)
			return;
		if (sound->IsPlaying())
			sound->Stop();
		sound->Play();
	}
	void load_wave_sound(CKWaveSound** sound, CKSTRING name, CKSTRING path, float gain = 1.0f, float pitch = 1.0f, bool streaming = false) {
		sound[0] = static_cast<CKWaveSound*>(m_bml->GetCKContext()->CreateObject(CKCID_WAVESOUND, name));
		sound[0]->Create(streaming, path);
		sound[0]->SetGain(gain);
		sound[0]->SetPitch(pitch);
	}

	std::set<CKWaveSound*> received_wave_sounds_;
	void destroy_wave_sound(CKWaveSound* sound, bool delete_file = false) {
		if (sound == nullptr) return;
		if (sound->IsPlaying())
			sound->Stop();
		std::string path = sound->GetSoundFileName();
		m_bml->GetCKContext()->DestroyObject(sound, CK_DESTROY_TEMPOBJECT);
		if (delete_file) DeleteFile(path.c_str());
	}
	void cleanup_received_sounds() {
		if (received_wave_sounds_.empty())
			return;
		for (const auto sound : received_wave_sounds_)
			destroy_wave_sound(sound, true);
		received_wave_sounds_.clear();
	}

	std::string get_display_nickname() {
		if (spectator_mode_)
			return bmmo::name_validator::get_spectator_nickname(db_.get_nickname());
		return db_.get_nickname();
	}

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

	bool update_current_sector() { // true if changed
		int sector = 0;
		if (ingame_parameter_array_ != 0) {
			static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(ingame_parameter_array_))->GetElementValue(0, 1, &sector);
			if (sector == current_sector_) return false;
		} else if (current_sector_ == 0) return false;
		current_sector_ = sector;
		current_sector_timestamp_ = db_.get_timestamp_ms();
		if (connected()) current_sector_timestamp_ += get_status().m_nPing;
		return true;
	}

	void update_sector_timestamp(const bmmo::map& map, int sector, int64_t timestamp) {
		if (sector == 0) return;
		auto map_it = maps_.find(map.get_hash_bytes_string());
		if (map_it == maps_.end()) return;
		map_it->second.sector_timestamps.try_emplace(sector, timestamp);
	}

	void resume_counter() {
		auto* mm = m_bml->GetMessageManager();
		CKMessageType unpause_level_msg = mm->AddMessageType("Unpause Level");
		mm->SendMessageSingle(unpause_level_msg, static_cast<CKBeObject*>(m_bml->GetCKContext()->GetObject(all_gameplay_beh_)));
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
		if (own_ball_visible_ == visible || spectator_mode_)
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
				std::lock_guard<std::mutex> lk(bml_mtx_);
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
						send_countdown_message(static_cast<bmmo::countdown_type>(i), countdown_mode_);
					}
				}
				if (input_manager_->IsKeyPressed(CKKEY_GRAVE)) {
					std::lock_guard<std::mutex> lk(bml_mtx_);
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
					send_dnf_message();
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
			std::lock_guard<std::mutex> lk(bml_mtx_);
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
		std::lock_guard<std::mutex> lk(bml_mtx_);
		client_cv_.notify_all();
		if (player_list_visible_) {
			player_list_visible_ = false;
			asio::post(thread_pool_, [this] {
				if (player_list_thread_.joinable()) player_list_thread_.join();
			});
		}
		if (down) {
			console_window_.hide();
			asio::post(thread_pool_, [this] {
				console_window_.free_thread();
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
		cleanup_received_sounds();

		{
			std::lock_guard client_lk(client_mtx_);
			for (const auto& i : logout_callbacks_)
				(i.second)();
		}

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

		m_bml->AddTimer(CKDWORD(3), [this]() {
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

	void send_countdown_message(bmmo::countdown_type type, bmmo::level_mode mode) {
		bmmo::countdown_msg msg{};
		msg.content.type = type;
		msg.content.mode = mode;
		msg.content.map = current_map_;
		msg.content.force_restart = reset_rank_;
		reset_rank_ = false;
		send(msg, k_nSteamNetworkingSend_Reliable);
	}

	void send_dnf_message() {
		bmmo::did_not_finish_msg msg{};
		msg.content.sector = current_sector_;
		msg.content.map = current_map_;
		msg.content.cheated = m_bml->IsCheatEnabled();
		send(msg, k_nSteamNetworkingSend_Reliable);
		did_not_finish_ = true;
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
		std::lock_guard<std::mutex> lk(client_mtx_);
		if (!spectator_mode_) update_sector_timestamp(current_map_, current_sector_, current_sector_timestamp_);
	}

	std::string get_username(HSteamNetConnection client_id) {
		if (client_id == k_HSteamNetConnection_Invalid)
			return {"[Server]"};
		auto state = db_.get(client_id);
		assert(state.has_value() || (db_.get_client_id() == client_id));
		return state.has_value() ? state->name : get_display_nickname();
	}

	void SendIngameMessage(const std::string& msg, int ansi_color = bmmo::ansi::Reset) {
		console_window_.print_text(msg.c_str(), ansi_color);
		utils_.call_sync_method([this, msg] { m_bml->SendIngameMessage(msg.c_str()); });
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
