#include <boost/regex.hpp>
#include <ranges>
// #include <openssl/sha.h>
// #include <fstream>
#include "CommandMMO.h"
#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
    BallanceMMOClient::init_socket();
    return new BallanceMMOClient(bml);
}

static VOID CALLBACK WinEventProcCallback(HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (!BallanceMMOClient::get_instance()) return;
    auto instance = BallanceMMOClient::get_instance();
    if (dwEvent == EVENT_SYSTEM_MOVESIZESTART) instance->enter_size_move();
    else if (dwEvent == EVENT_SYSTEM_MOVESIZEEND) instance->exit_size_move();
}

void BallanceMMOClient::show_player_list() {
    // player_list_thread has a sleep_for and may freeze the game if joined synchronously
    asio::post(thread_pool_, [this] {
        stop_player_list(true); // we are on a pool thread, so joining here is fine
        if (shutting_down_) return;
        utils_.schedule_sync_call([this] {
            player_list_display_ = std::make_unique<decltype(player_list_display_)::element_type>("PlayerList", "", RIGHT_MOST, 0.412f);
            player_list_display_->sprite_->SetPosition({0.596f, 0.412f});
            player_list_display_->sprite_->SetSize({RIGHT_MOST - 0.596f, 0.588f});
            player_list_display_->sprite_->SetZOrder(96);
            player_list_display_->paint(player_list_color_);
            // player_list_display_->paint_background(0x44444444);
            player_list_display_->set_visible(true);
            std::lock_guard thread_lk(player_list_thread_mtx_);
            player_list_thread_ = utils::create_named_thread(L"BMMO_PlayerList", [this] {
                player_list_visible_ = true;
                player_list_last_count_ = -1;
                player_list_last_font_size_ = -1;
                last_player_list_text_.clear();
                while (player_list_visible_) {
                    update_player_list();
                    // wait on the cv instead of sleeping: stopping this thread
                    // must not take up to half a second while BML tears down
                    std::unique_lock lk(player_list_cv_mtx_);
                    player_list_cv_.wait_for(lk, std::chrono::milliseconds(500),
                                             [this] { return !player_list_visible_; });
                }
                // BML is already going away; scheduling a call into it now would
                // run against a destroyed mod
                if (!shutting_down_)
                    utils_.schedule_sync_call([this] { player_list_display_.reset(); });
            });
        });
    });
}

inline void BallanceMMOClient::update_player_list() {
    auto list_sorter = [](const player_status_list_entry& i1, const player_status_list_entry& i2) {
        const int map_cmp = boost::to_lower_copy(i1.map_name).compare(boost::to_lower_copy(i2.map_name));
        if (map_cmp > 0) return true;
        if (map_cmp < 0) return false;
        const int sector_cmp = i1.sector - i2.sector;
        if (sector_cmp != 0) return sector_cmp > 0;
        if (i1.sector != 1) {
            const int64_t time_cmp = i1.time_diff - i2.time_diff;
            if (time_cmp != 0) return time_cmp < 0;
        }
        return boost::ilexicographical_compare(i1.name, i2.name);
    };
    // only display the time diff if there's at least 2 players
    // (including the player that reaches it first) in the sector
    enum timestamp_diff_status { TdsNone = 0, TdsZero = 0b01, TdsNonZero = 0b10, TdsDisplay = 0b11 };

    std::unordered_map<std::string, std::map<int, int>> timestamp_display; // std::map<int, tds>
    // This runs on the player-list thread, which may not call BML APIs or read
    // game-thread state; everything engine-owned comes from the snapshot the
    // game thread publishes each frame.
    const auto snapshot = get_player_list_snapshot();
    std::unique_lock lk(player_status_list_mtx_);
    player_status_list_.clear();
    player_status_list_.reserve(db_.player_count() + !snapshot.spectator);
    auto push_entry = [&, this](const bmmo::map& map, const std::string& name, int sector, int64_t timestamp, bool cheated) {
        // if_contains holds the submap lock for the lookup, and unlike
        // operator[] it does not insert a map entry from this thread
        int64_t sector_timestamp = 0;
        bool has_timestamp = false;
        maps_.if_contains(map.get_hash_bytes_string(), [&](const auto& entry) {
            const auto& times = entry.second.sector_timestamps;
            if (auto time_it = times.find(sector); time_it != times.end()) {
                sector_timestamp = time_it->second;
                has_timestamp = true;
            }
        });
        auto map_name = get_map_name(map);
        if (has_timestamp) {
            timestamp -= sector_timestamp;
            timestamp_display[map_name][sector] |= (timestamp == 0 ? TdsZero : TdsNonZero);
        }
        player_status_list_.push_back({ map_name, name, sector, timestamp, cheated });
    };

    db_.for_each([&](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
        if (pair.first == db_.get_client_id() || bmmo::name_validator::is_spectator(pair.second.name))
            return true;
        push_entry(pair.second.current_map, pair.second.name, pair.second.current_sector,
                    pair.second.current_sector_timestamp, pair.second.cheated);
        return true;
    });
    if (!snapshot.spectator)
        push_entry(snapshot.current_map, get_display_nickname(), snapshot.sector,
                    snapshot.sector_timestamp, snapshot.cheated);
    std::ranges::sort(player_status_list_, list_sorter);
    auto size = int(player_status_list_.size());

    std::string text = std::to_string(size) + " player" + ((size == 1) ? "" : "s") + " online:\n";
    text.reserve(1024);
    std::string last_map_name;
    int ranking = 0;
    const auto& own_map_name = snapshot.own_map_display_name;
    for (const auto& i: player_status_list_ /* | std::views::reverse */) {
        std::string map_display_name;
        if (last_map_name != i.map_name)
            last_map_name = map_display_name = i.map_name;
        else
            map_display_name = "~";
        bool show_rankings = snapshot.show_rankings && i.map_name == own_map_name;
        if (show_rankings)
            ++ranking;
        std::string time_diff_str;
        const auto& time_diff_map = timestamp_display[i.map_name];
        if (auto time_it = time_diff_map.find(i.sector);
                time_it != time_diff_map.end() && i.sector > 1
                && time_it->second == TdsDisplay)
            time_diff_str = std::abs(i.time_diff) < 100000
                    ? std::format(" {:+06.2f}s", float(i.time_diff) / 1000)
                    : std::format(" {:+#06.4g}s", float(i.time_diff) / 1000);
        text.append(std::format("{}" "{}{}: {}, S{:02d}{}\n",
                show_rankings ? std::format("({}) ", ranking) : "",
                i.cheated ? "[C] " : "", i.name, map_display_name, i.sector, time_diff_str));
    }
    if (text == last_player_list_text_) return; // only update if changed; lock auto released here
    last_player_list_text_ = text;
    lk.unlock();

    // capture by value + members only: this runs later on the game thread,
    // possibly after the player-list thread that called us has exited
    utils_.schedule_sync_call([this, size] {
        std::unique_lock lk(player_status_list_mtx_);
        if (size != player_list_last_count_) {
            player_list_last_count_ = size;
            auto font_size = utils_.get_display_font_size(10.9f - (1.0f / 6) * std::clamp(size, 7, 29));
            if (player_list_last_font_size_ != font_size) {
                player_list_last_font_size_ = font_size;
                if (player_list_display_)
                    player_list_display_->sprite_->SetFont(utils::get_system_font(), font_size, 400, false, false);
            }
        }
        if (player_list_display_)
            player_list_display_->update(bmmo::string_utils::utf8_to_ansi(last_player_list_text_));
    });
}

inline void BallanceMMOClient::enter_size_move() {
    if (current_level_mode_ != bmmo::level_mode::Highscore || !m_bml->IsIngame()) return;
    last_move_size_time_ = m_bml->GetTimeManager()->GetTime();
}

inline void BallanceMMOClient::exit_size_move() {
    if (current_level_mode_ != bmmo::level_mode::Highscore || !m_bml->IsIngame()) return;
    auto last_length = move_size_time_length_;
    move_size_time_length_ += m_bml->GetTimeManager()->GetTime() - last_move_size_time_;
    auto array = static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(energy_array_));
    int points; array->GetElementValue(0, 0, &points);
    points -= int(std::ceilf(move_size_time_length_ / point_decrease_interval_) - std::ceilf(last_length / point_decrease_interval_));
    array->SetElementValue(0, 0, &points);
}

void BallanceMMOClient::OnLoad()
{
    // BML callbacks all run on the game thread; remember it so code reached
    // from the network thread can tell when it must defer to run_on_game_thread
    utils_.set_game_thread();
    config_manager_.init_config();
    objects_.toggle_extrapolation(config_manager_["extrapolation"]->GetBoolean());
    parse_and_set_player_list_color(config_manager_["player_list_color"]);
    objects_.toggle_dynamic_opacity(config_manager_["dynamic_opacity"]->GetBoolean());
    sound_enabled_ = config_manager_["sound_notification"]->GetBoolean();
    ignore_forced_sounds_ = config_manager_["mute_everything"]->GetBoolean();

    init_commands();
    //client_ = std::make_unique<client>(logger_, m_bml);
    input_manager_ = m_bml->GetInputManager();
    load_wave_sound(&sound_countdown_, "MMO_Sound_Countdown", "..\\Sounds\\Menu_dong.wav", 0.88f);
    load_wave_sound(&sound_go_, "MMO_Sound_Go", "..\\Sounds\\Menu_dong.wav", 1.0f, 1.75f);
    load_wave_sound(&sound_level_finish_, "MMO_Sound_Level_Finish", "..\\Sounds\\Music_Highscore.wav", 0.16f);
    load_wave_sound(&sound_level_finish_cheat_, "MMO_Sound_Level_Finish_Cheat", "..\\Sounds\\Hit_Stone_Wood.wav", 0.27f, 0.5f * std::powf(2.0f, 9.0f / 12));
    load_wave_sound(&sound_dnf_, "MMO_Sound_DNF", "..\\Sounds\\Misc_RopeTears.wav", 0.9f);
    load_wave_sound(&sound_notification_, "MMO_Sound_Notification", "..\\Sounds\\Hit_Stone_Kuppel.wav", 0.76f);
    load_wave_sound(&sound_bubble_, "MMO_Sound_Bubble", "..\\Sounds\\Extra_Life_Blob.wav", 0.88f);
    load_wave_sound(&sound_knock_, "MMO_Sound_Knock", "..\\Sounds\\Pieces_Stone.wav", 0.88f, 0.88f);

    utils::cleanup_old_crash_dumps();
    start_command_pipe_from_environment();
    install_input_hook();

    using namespace std::placeholders;
    m_bml->RegisterCommand(new CommandMMO(std::bind(&BallanceMMOClient::OnCommand, this, _1, _2), std::bind(&BallanceMMOClient::OnTabComplete, this, _1, _2)));
    m_bml->RegisterCommand(new CommandMMOSay(std::bind(&BallanceMMOClient::OnCommand, this, _1, _2), std::bind(&BallanceMMOClient::OnTabComplete, this, _1, _2)));

    for (const auto& file_data: bmmo::HASHES_TO_CHECK) {
        auto& hash_data = md5_data_[file_data[0]];
        utils::md5_from_file(std::string{"..\\"}.append(file_data[0]), hash_data.data());
    }

    // Design 9.10: route the retail "Random" block through the bridge's
    // deterministic generator, wherever the bridge is available (the open
    // source physics_RT.dll only - the retail one has no bridge at all).
    {
        std::string error;
        if (physics_view_.available() || physics_view_.initialize(m_bml->GetCKContext(), error)) {
            const int patched = physics_view_.install_random_block(error);
            logger_->Info("Random block hooked (%d instances)", patched);
        } else {
            logger_->Info("Random block hook unavailable: %s", error.c_str());
        }
    }

#ifdef BMMO_WITH_PLAYER_SPECTATION
    spect_cam_ = static_cast<CKCamera*>(m_bml->GetCKContext()->CreateObject(CKCID_CAMERA, "Spectator_Cam"));
#endif
}

void BallanceMMOClient::OnLoadObject(BMMO_CKSTRING filename, BOOL isMap, BMMO_CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj)
{
    if (isMap) {
        logger_->Info("Initializing peer objects...");
        objects_.init_players();
        boost::regex name_pattern("^.*\\\\(Level|Maps)\\\\(.*).[cn]mo$", boost::regex::icase);
        std::string path(filename);
        boost::smatch matched;
        if (boost::regex_search(path, matched, name_pattern)) {
            current_map_.name = matched[2].str();
            if (boost::iequals(matched[1].str(), "Maps")) {
                current_map_.type = bmmo::map_type::CustomMap;
                if (auto slash_pos = current_map_.name.rfind('\\'); slash_pos != std::string::npos)
                    current_map_.name.erase(0, slash_pos + 1);
            }
            else {
                current_map_.type = bmmo::map_type::OriginalLevel;
                path = "..\\" + path;
            }
        } else {
            current_map_.name = filename;
            current_map_.type = bmmo::map_type::Unknown;
        }
#ifdef BMMO_USE_BML_PLUS // BMLPlus >= 0.3.4 uses UTF-8 for paths but our hash function requires ANSI
        if (loader_version_ >= BMLVersion{ 0, 3, 4 }) {
            path = bmmo::string_utils::utf8_to_ansi(path);
        }
        else
#endif
        {
            current_map_.name = bmmo::string_utils::ansi_to_utf8(current_map_.name);
        }
        utils::md5_from_file(path, current_map_.md5);
        static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(current_level_array_))->GetElementValue(0, 0, &current_map_.level);
        current_level_mode_ = bmmo::level_mode::Speedrun;
        did_not_finish_ = false;
        max_sector_ = 0;
        if (connected()) {
            const auto hash = current_map_.get_hash_bytes_string();
            if (!has_map_name(hash)) {
                add_map_name(hash, current_map_.name);
                send_current_map_name();
            } else
                current_map_.name = get_raw_map_name(hash);
            player_ball_ = get_current_ball();
            if (player_ball_ != nullptr) {
                local_state_handler_->poll_and_send_state_forced(player_ball_);
            }
            on_sector_changed();
            send_current_map();
        };
        map_enter_timestamp_ = SteamNetworkingUtils()->GetLocalTimestamp();
        hs_begin_delay_ = 0;
        logger_->Info("Current map: %s; type: %d; md5: %s.",
            current_map_.name.c_str(), (int)current_map_.type, current_map_.get_hash_string().c_str());
        reset_timer_ = true;
        logger_->Info("Initialization completed.");
    }
    /*if (isMap) {
        std::string filename_string(filename);
        std::filesystem::path path = std::filesystem::current_path().parent_path().append(filename_string[0] == '.' ? filename_string.substr(3, filename_string.length()) : filename_string);
        std::ifstream map(path, std::ios::in | std::ios::binary);
        map_hash_ = hash_sha256(map);
        SendIngameMessage(map_hash_.c_str());
        blcl::net::message<MsgType> msg;
        msg.header.id = MsgType::EnterMap;
        client_.send(msg);
    }*/

    else if (strcmp(filename, "3D Entities\\Balls.nmo") == 0) {
        objects_.destroy_all_objects();
        objects_.init_template_balls();
        //objects_.init_players();
    }
    else if (strcmp(filename, "3D Entities\\Gameplay.nmo") == 0) {
        current_level_array_ = CKOBJID(m_bml->GetArrayByName("CurrentLevel"));
        ingame_parameter_array_ = CKOBJID(m_bml->GetArrayByName("IngameParameter"));
    }

    // Design 9.10: the hook only covers the Random blocks inside the trafo
    // explosion scripts, which arrive with 3D Entities\Balls.nmo after
    // OnLoad; the pass is idempotent (the session anchor repeats it too).
    if (physics_view_.available()) {
        std::string error;
        const int patched = physics_view_.install_random_block(error);
        if (patched > 0) logger_->Info("Random block hooked (%d instances)", patched);
    }
}

void BallanceMMOClient::on_sector_changed() {
    if (!update_current_sector()) return;
    max_sector_ = std::max(current_sector_.load(), max_sector_.load());
    if (connected()) send_current_sector();
    physics_session_on_sector(current_sector_);
    if (current_level_mode_ != bmmo::level_mode::Highscore) return;
    extra_life_received_ = false;
}

void BallanceMMOClient::OnPostCheckpointReached() { on_sector_changed(); }

void BallanceMMOClient::OnPostExitLevel() {
    physics_session_end_local("left the level");
    ball_nav_active_ = false;
    countdown_restart_ = false;
    force_hs_calibration_ = false;
    if (current_level_mode_ == bmmo::level_mode::Highscore && !spectator_mode_) {
        /*if (!level_finished_ && !did_not_finish_)
            send_dnf_message();*/
        level_finished_ = false;
        compensation_lives_label_.reset();
    }
    on_sector_changed();
    if (spectator_label_) spectator_label_->update("[Spectator Mode]");
}

void BallanceMMOClient::OnPreResetLevel() {
    if (spectator_mode_) return;
    const auto it = maps_.find(last_countdown_map_.get_hash_bytes_string());
    if (it == maps_.end()) return;
    if (m_bml->GetTimeManager()->GetTime() - it->second.level_start_timestamp
            < bmmo::LEVEL_RESTART_IGNORE_TIMEFRAME_MS)
        return;
    send(bmmo::simple_action_msg{.content = bmmo::simple_action::LevelRestarted}, k_nSteamNetworkingSend_Reliable);
}

void BallanceMMOClient::OnCounterActive() {
    on_sector_changed();
    bool reset_counter = true;
    if (map_enter_timestamp_ != 0) {
        // std::lock_guard lk(bml_mtx_);
        if (force_hs_calibration_ && !hs_calibrated_) {
            int points;
            auto* energy = static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(energy_array_));
            energy->GetElementValue(0, 0, &points);
            points -= (int) std::ceilf(hs_begin_delay_ / 1e3f / point_decrease_interval_);
            energy->SetElementValue(0, 0, &points);
            counter_start_timestamp_ = -(hs_begin_delay_ / 1e3f);
            reset_counter = false;
            countdown_restart_ = true;
            hs_calibrated_ = true;
        } else {
            hs_begin_delay_ -= (SteamNetworkingUtils()->GetLocalTimestamp() - map_enter_timestamp_);
        }
        force_hs_calibration_ = false;
        map_enter_timestamp_ = 0;
    }
    if (countdown_restart_) {
        move_size_time_length_ = 0;
        if (reset_counter) counter_start_timestamp_ = 0;
        counter_start_timestamp_ += m_bml->GetTimeManager()->GetTime();
        countdown_restart_ = false;
    }
}

void BallanceMMOClient::OnPostStartMenu()
{
    if (init_) {
        logger_->Info("Destroying peer objects...");
        objects_.destroy_all_objects();
        logger_->Info("Destroy completed.");
    }
    else {
        ping_ = std::make_shared<text_sprite>("T_MMO_PING", "", RIGHT_MOST, 0.03f);
        ping_->sprite_->SetSize(Vx2DVector(RIGHT_MOST, 0.4f));
        ping_->sprite_->SetFont("Arial", utils_.get_display_font_size(10), 500, false, false);
        status_ = std::make_shared<text_sprite>("T_MMO_STATUS", "Disconnected", RIGHT_MOST, 0.0f);
        status_->sprite_->SetFont("Times New Roman", utils_.get_display_font_size(11), 700, false, false);
        status_->paint(0xffff0000);

        edit_Gameplay_Tutorial(m_bml->GetScriptByName("Gameplay_Tutorial"));

        config_manager_.validate_nickname();
        db_.set_nickname(bmmo::string_utils::ansi_to_utf8(config_manager_["playername"]->GetString()));

        auto* energy_array_ptr = m_bml->GetArrayByName("Energy");
        energy_array_ = CKOBJID(energy_array_ptr);
        energy_array_ptr->GetElementValue(0, 2, &initial_points_);
        energy_array_ptr->GetElementValue(0, 3, &initial_lives_);
        energy_array_ptr->GetElementValue(0, 4, &point_decrease_interval_);
        // SendIngameMessage(std::to_string(m_bml->GetParameterManager()->GetParameterSize(m_bml->GetParameterManager()->ParameterGuidToType(energy_array_ptr->GetColumnParameterGuid(4)))));

        all_gameplay_beh_ = CKOBJID(static_cast<CKBeObject*>(m_bml->GetCKContext()->GetObjectByNameAndParentClass("All_Gameplay", CKCID_BEOBJECT, nullptr)));

        move_size_hook_ = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, NULL,
                                          WinEventProcCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
        server_list_ = std::make_unique<server_list>(m_bml, &log_manager_, &config_manager_,
                                       [this](auto addr, auto name) { connect_to_server(addr, name); });
        server_list_->init_gui();

        init_ = true;
    }
}

void BallanceMMOClient::OnProcess() {
    process_command_file();
    fixed_tick_.on_process(m_bml);
    process_physics_session();
    process_command_pipe();
    process_level_request();
    process_tick_record();
    poll_local_input();
    if (init_)
        server_list_->process();

    {
        std::lock_guard lk(ingame_msg_mtx_);
        while (!ingame_msg_queue_.empty()) {
            m_bml->SendIngameMessage(ingame_msg_queue_.front().c_str());
            ingame_msg_queue_.pop();
        }
    }

    // refresh the values worker threads read instead of querying the engine
    utils_.refresh_engine_cache();
    own_cheat_enabled_ = m_bml->IsCheatEnabled();
    spectator_mode_ = config_manager_["spectator"]->GetBoolean();

    if (!connected())
        return;

    apply_pending_ping_text();
    // publish engine-owned state for the player-list thread, which runs on its
    // own thread and must not call BML APIs itself
    if (player_list_visible_)
        refresh_player_list_snapshot();

    std::unique_lock bml_lk(bml_mtx_, std::try_to_lock);

    if (!(m_bml->IsIngame() && bml_lk))
        return;
    const auto current_timestamp = SteamNetworkingUtils()->GetLocalTimestamp();

    {
        std::lock_guard lk(frame_times_mtx_);
        frame_timestamps_in_last_45s_.push(current_timestamp);
        while (!frame_timestamps_in_last_45s_.empty()
                && current_timestamp - frame_timestamps_in_last_45s_.front() > 45 * (SteamNetworkingMicroseconds)1e6)
            frame_timestamps_in_last_45s_.pop();
    }

    const bool flush = db_.flush();
    if (!objects_.update(current_timestamp, flush) && flush)
        db_.set_pending_flush(true);

    if (current_timestamp >= next_update_timestamp_) {
        if (current_timestamp - next_update_timestamp_ > 1048576)
            next_update_timestamp_ = current_timestamp;
        next_update_timestamp_ += bmmo::CLIENT_MINIMUM_UPDATE_INTERVAL_US;

        auto ball = get_current_ball();
        if (player_ball_ == nullptr)
            player_ball_ = ball;

        check_on_trafo(ball);
        local_state_handler_->poll_and_send_state(ball);
    }

    if (compensation_lives_label_) compensation_lives_label_->process();

#ifdef BMMO_WITH_PLAYER_SPECTATION
    if (spectating_first_person_) {
        if (!db_.exists(objects_.get_spectated_id())) {
            OnFullCommand("spectate"); // just exit spectation
            return;
        }
        spect_player_pos_ = objects_.get_ball_pos(objects_.get_spectated_id());
        if (input_manager_->IsKeyDown(CKKEY_LSHIFT)) {
            CKCamera* cam = m_bml->GetTargetCameraByName("InGameCam");
            VxVector cam_pos, ball_pos;
            cam->GetPosition(&cam_pos);
            player_ball_->GetPosition(&ball_pos);
            spect_pos_diff_ = cam_pos - ball_pos;
            VxQuaternion rot;
            cam->GetQuaternion(&rot);
            spect_cam_->SetQuaternion(VT21_REF(rot));
        }
        VxVector current_cam_pos;
        spect_cam_->GetPosition(&current_cam_pos);
        auto delta = m_bml->GetTimeManager()->GetLastDeltaTime();
        current_cam_pos = Interpolate(0.006f * delta, current_cam_pos, spect_player_pos_ + spect_pos_diff_)
                - VxVector(0, 0.00492f * delta * (spect_player_pos_.y + spect_pos_diff_.y - current_cam_pos.y), 0);
        spect_cam_->SetPosition(VT21_REF(current_cam_pos));
        spect_target_pos_ = Interpolate(0.011f * delta, spect_target_pos_, spect_player_pos_);
        spect_cam_->LookAt(VT21_REF(spect_target_pos_));
        VxVector dir, up;
        spect_cam_->GetOrientation(&dir, &up);
        if (up.y < 0) { // happens when the camera is upside-down
            up = -up;
            spect_cam_->SetOrientation(VT21_REF(dir), VT21_REF(up));
        }
    }
#endif
}

void BallanceMMOClient::OnStartLevel()
{
    /*if (!connected())
        return;*/

    player_ball_ = get_current_ball();
    if (local_state_handler_ != nullptr)
        local_state_handler_->set_ball_type(db_.get_ball_id(player_ball_->GetName()));

    if (reset_timer_) {
        maps_[current_map_.get_hash_bytes_string()].level_start_timestamp = m_bml->GetTimeManager()->GetTime();
        reset_timer_ = false;
    }

    if (!connected()) countdown_restart_ = true;

    level_finished_ = false;
    ball_off_ = false;
    extra_life_received_ = false;
    compensation_lives_ = 0;
    if (compensation_lives_label_) compensation_lives_label_.reset();
    hs_begin_delay_ += SteamNetworkingUtils()->GetLocalTimestamp() - map_enter_timestamp_;
    hs_calibrated_ = false;

    m_bml->AddTimer(CKDWORD(10), [this]() {
        if (auto maps_it = maps_.find(current_map_.get_hash_bytes_string()); maps_it != maps_.end()) {
            add_lives(maps_it->second.initial_life_count);
        }

        if (current_map_.level == 1 && countdown_restart_ && connected()) {
            auto* tutorial_exit = static_cast<CKBehaviorIO*>(m_bml->GetCKContext()->GetObject(tutorial_exit_event_));
            tutorial_exit->Activate();
        }

        if (!countdown_restart_ && current_level_mode_ == bmmo::level_mode::Highscore && map_enter_timestamp_ == 0) {
            std::lock_guard lk(bml_mtx_);
            int new_points = (current_map_.level == 1) ? initial_points_ : initial_points_ - 1;
            new_points -= (int) std::ceilf((m_bml->GetTimeManager()->GetTime() - counter_start_timestamp_) / point_decrease_interval_);
            static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(energy_array_))->SetElementValue(0, 0, &new_points);
            resume_counter();
            hs_calibrated_ = true;
        }

        on_sector_changed();
    });

    //objects_.destroy_all_objects();
    //objects_.init_players();
}

void BallanceMMOClient::OnPauseLevel() {
    if (current_level_mode_ == bmmo::level_mode::Highscore) resume_counter();
}

void BallanceMMOClient::OnBallOff() {
    if (current_level_mode_ == bmmo::level_mode::Highscore) resume_counter();
    ball_off_ = true;
    if (!spectator_mode_)
        send(bmmo::simple_action_msg{ .content = bmmo::simple_action::BallOff }, k_nSteamNetworkingSend_Reliable);
}

void BallanceMMOClient::OnCamNavActive() {
    if (!ball_off_ || current_level_mode_ != bmmo::level_mode::Highscore)
        return;
    if (extra_life_received_) {
        extra_life_received_ = false;
        return;
    }
    ++compensation_lives_;
    update_compensation_lives_label();
    ball_off_ = false;
    extra_life_received_ = false;
}

void BallanceMMOClient::OnPreLifeUp() {
    if (current_level_mode_ != bmmo::level_mode::Highscore) return;
    extra_life_received_ = true;
}

void BallanceMMOClient::update_compensation_lives_label() {
    if (compensation_lives_ != 0 && !compensation_lives_label_) {
        compensation_lives_label_ = std::make_unique<label_sprite>("Compensation_Lives", "", 0.8f, 0.834f);
        auto sprite = compensation_lives_label_->sprite_.get();
        sprite->SetAlignment(ALIGN_BOTTOMRIGHT);
        sprite->SetFont(ExecuteBB::GAMEFONT_02);
        sprite->SetSize({ 0.114f, 0.05f });
        compensation_lives_label_->set_visible(true);
    }
    compensation_lives_label_->update(std::format("+{}", compensation_lives_));
}

// may give wrong values of extra points
void BallanceMMOClient::OnLevelFinish() {
    physics_session_on_finish();
    if (!connected() || spectator_mode_)
        return;

    if (did_not_finish_) {
        SendIngameMessage("Not sending level completion messages as you have already forfeited it.");
        return;
    }
    auto* array_energy = static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(energy_array_));
    int lives; array_energy->GetElementValue(0, 1, &lives);
    lives += compensation_lives_;
    array_energy->SetElementValue(0, 1, &lives);
    level_finished_ = true;

    // Sending level finish messages immediately may get us wrong values of
    // extra points. We have to wait for some time.
    // The float overload of AddTimer counts real milliseconds rather than
    // frames, so this is not unfair to players with low framerates - and
    // unlike the sleeping thread-pool task it used to be, it reads the energy
    // and level arrays on the game thread.
    m_bml->AddTimer(100.0f, [this]() {
        auto* array_energy = static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(energy_array_));
        auto* array_level = static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(current_level_array_));
        auto* all_levels = m_bml->GetArrayByName("AllLevel");
        if (!array_energy || !array_level || !all_levels)
            return;
        bmmo::level_finish_v2_msg msg{};
        array_energy->GetElementValue(0, 0, &msg.content.points);
        array_energy->GetElementValue(0, 1, &msg.content.lives);
        array_energy->GetElementValue(0, 5, &msg.content.lifeBonus);
        array_level->GetElementValue(0, 0, &current_map_.level);
        all_levels->GetElementValue(current_map_.level - 1, 6, &msg.content.levelBonus);
        msg.content.timeElapsed = (m_bml->GetTimeManager()->GetTime() - maps_[current_map_.get_hash_bytes_string()].level_start_timestamp) / 1e3f;
        reset_timer_ = true;
        msg.content.cheated = m_bml->IsCheatEnabled();
        msg.content.mode = current_level_mode_;
        msg.content.map = current_map_;
        logger_->Info("Sending level finish message...");

        send(msg, k_nSteamNetworkingSend_ReliableNoNagle);
    });
}

void BallanceMMOClient::OnLoadScript(BMMO_CKSTRING filename, CKBehavior* script)
{
    if (strcmp(script->GetName(), "Gameplay_Ingame") == 0)
        edit_Gameplay_Ingame(script);
    if (strcmp(script->GetName(), "Gameplay_Events") == 0)
        edit_Gameplay_Events(script);
    if (strcmp(script->GetName(), "Gameplay_Energy") == 0)
        edit_Gameplay_Energy(script);
    // Weird. Gameplay_Tutorial doesn't trigger OnLoadScript, so we have to
    // get it from the PostStartMenu event.
    // if (strcmp(script->GetName(), "Gameplay_Tutorial") == 0)
    //     edit_Gameplay_Tutorial(script);
    if (strcmp(script->GetName(), "Event_handler") == 0)
        edit_Event_handler(script);
    if (strcmp(script->GetName(), "Menu_Pause") == 0)
        edit_Menu_Pause(script);
}

void BallanceMMOClient::OnCheatEnabled(bool enable) {
    own_cheat_enabled_ = enable; // keep the network thread's mirror current
    if (!connected() || spectator_mode_)
        return;
    bmmo::cheat_state_msg msg{};
    msg.content.cheated = enable;
    msg.content.notify = notify_cheat_toggle_;
    send(msg, k_nSteamNetworkingSend_Reliable);
}

void BallanceMMOClient::OnModifyConfig(BMMO_CKSTRING category, BMMO_CKSTRING key, IProperty* prop) {
    if (prop == config_manager_["playername"]) {
        if (!config_manager_.check_and_set_nickname(prop, db_))
            return;
    }
    else if (prop == config_manager_["extrapolation"]) {
        objects_.toggle_extrapolation(prop->GetBoolean());
        return;
    }
    else if (prop == config_manager_["dynamic_opacity"]) {
        objects_.toggle_dynamic_opacity(prop->GetBoolean());
        return;
    }
    else if (prop == config_manager_["player_list_color"]) {
        parse_and_set_player_list_color(prop);
        return;
    }
    else if (prop == config_manager_["sound_notification"]) {
        sound_enabled_ = prop->GetBoolean();
        return;
    }
    else if (prop == config_manager_["mute_everything"]) {
        ignore_forced_sounds_ = prop->GetBoolean();
        return;
    }
    if (connected() || connecting()) {
        disconnect_from_server();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        connect_to_server(server_addr_.c_str(), server_name_.c_str());
    }
}

void BallanceMMOClient::OnExitGame()
{
    command_pipe_.stop();
    UnhookWinEvent(move_size_hook_);
    config_manager_.check_and_save_name_change_time();
    cleanup(true);
    client::destroy();
}

inline void BallanceMMOClient::on_fatal_error(char* extra_text) {
    if (!connected())
        return;

    /*if (current_level_mode_ == bmmo::level_mode::Highscore
            && !spectator_mode_ && !level_finished_ && !did_not_finish_) {
        send_dnf_message();
        extra_text = std::format("You did not finish {}.\nFurthest reach: sector {}.",
                                 current_map_.get_display_name(), max_sector_);
    }*/
    bmmo::simple_action_msg msg{};
    msg.content = bmmo::simple_action::FatalError;
    send(msg, k_nSteamNetworkingSend_Reliable);
}

//void BallanceMMOClient::OnUnload() {
//    cleanup(true);
//    client::destroy();
//}

void BallanceMMOClient::OnCommand(IBML* bml, const std::vector<std::string>& args)
{
    // discard the first "mmo" argument
    const std::string full_command = bmmo::string_utils::join_strings(args, 1);
    OnFullCommand(full_command);
}

void BallanceMMOClient::OnFullCommand(const std::string& full_command)
{
    // Commands arrive from the in-game command bar (game thread), the console
    // window's input thread and OnAsyncCommand's pool task. Many of them touch
    // the engine (spawning spirit balls, sending CK messages, reading cheat
    // state), so run the whole dispatch on the game thread; when we are already
    // there this is a plain call.
    if (!utils_.on_game_thread()) {
        utils_.schedule_sync_call([this, full_command] { OnFullCommand(full_command); });
        return;
    }

    auto help = [this](IBML* bml) {
        std::lock_guard lk(bml_mtx_);
        SendIngameMessage("BallanceMMO Help", bmmo::ansi::Bold);
        SendIngameMessage(std::format("Version: {}; build time: {}.",
                                      version_string, bmmo::string_utils::get_build_time_string()));
        SendIngameMessage("/mmo connect - Connect to server.");
        SendIngameMessage("/mmo disconnect - Disconnect from server.");
        SendIngameMessage("/mmo list - List online players.");
        SendIngameMessage("/mmo say - Send message to each other.");
        SendIngameMessage("Full subcommand list:");
        auto cmd_str = console_.get_help_string();
        while (!cmd_str.empty()) {
            auto pos = cmd_str.find(',', 80);
            pos += (pos == std::string::npos) ? 0 : 2;
            SendIngameMessage(cmd_str.substr(0, pos));
            cmd_str.erase(0, pos);
        }
    };

    if (console_.execute(full_command))
        return;
    if (console_.get_command_name().empty())
        help(m_bml);
    else {
        std::string extra_text;
        if (auto hints = console_.get_command_hints(true); !hints.empty())
            extra_text = " Did you mean: " + bmmo::string_utils::join_strings(hints, 0, ", ") + "?";
        SendIngameMessage(std::format("Error: unknown subcommand \"{}\".{}",
                                      console_.get_command_name(), extra_text));
    }
}

void BallanceMMOClient::OnAsyncCommand(IBML* bml, const std::vector<std::string>& args) {
    asio::post(thread_pool_, [=, this] { OnCommand(bml, args); });
}

void BallanceMMOClient::init_commands() {
    /*console_.register_command("p", [&] { objects_.physicalize_all(); });
    console_.register_command("f", [&] { ExecuteBB::SetPhysicsForce(player_ball_, VxVector(0, 0, 0), player_ball_, VxVector(1, 0, 0), m_bml->Get3dObjectByName("Cam_OrientRef"), .43f); });
    console_.register_command("u", [&] { ExecuteBB::UnsetPhysicsForce(player_ball_); });*/
    console_.set_completion_callback([this](const std::vector<std::string>& args) {
        std::vector<std::string> args_copy(args);
        if (args.size() > 0 && (args[0] != "mmo" && args[0] != "ballancemmo"))
            args_copy.insert(args_copy.begin(), "ballancemmo");
        return OnTabComplete(m_bml, args_copy);
    });

    static auto get_client_id_from_console = [this] {
        auto next_word = console_.get_next_word();
        if (next_word.empty()) return k_HSteamNetConnection_Invalid;
        if (next_word[0] == '#')
            return (HSteamNetConnection) std::atoll(next_word.substr(1).c_str());
        return (next_word == get_display_nickname()) ? db_.get_client_id() : db_.get_client_id(next_word);
    };

    console_.register_command("say", [&] {
        bmmo::chat_msg msg{};
        msg.chat_content = console_.get_rest_of_line();

        const char* new_text = nullptr;
        bool canceled = false;
        notify_listeners([&](const auto& i) { canceled |= !i->on_pre_chat(msg.chat_content.c_str(), &new_text); });
        if (canceled) return;

        if (new_text && std::strlen(new_text) < UINT16_MAX)
            msg.chat_content.assign(new_text);

        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        return;
    });
    console_.register_aliases("say", {"s"});
    console_.register_command("announce", [&] {
        using in_msg = bmmo::important_notification_msg;
        in_msg msg{};
        msg.chat_content = console_.get_rest_of_line();
        msg.type = (console_.get_command_name() == "notice") ? in_msg::Notice : in_msg::Announcement;
        msg.serialize();

        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        return;
    });
    console_.register_aliases("announce", {"a", "notice"});
    console_.register_command("bulletin", [&] {
        bmmo::permanent_notification_msg msg{};
        msg.text_content = console_.get_rest_of_line();
        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    });
    console_.register_aliases("bulletin", {"b"});
    console_.register_command("kick", [&] {
        bmmo::kick_request_msg msg{};
        if (console_.get_command_name() == "crash")
            msg.crash = true;
        auto next_word = console_.get_next_word();
        if (next_word.empty()) return;
        if (next_word[0] == '#')
            msg.player_id = (HSteamNetConnection) atoll(next_word.substr(1).c_str());
        else
            msg.player_name = next_word;
        msg.reason = console_.get_rest_of_line();

        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    });
    console_.register_aliases("kick", {"crash"});
    console_.register_command("remotecommand", [&] {
        bmmo::remote_command_msg msg{};
        msg.text_content = console_.get_rest_of_line();
        if (msg.text_content.empty()) return;
        SendIngameMessage(std::format("Sent remote command: {}", msg.text_content), bmmo::ansi::WhiteInverse);
        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    });
    console_.register_aliases("remotecommand", {"rc"});
    console_.register_command("restartlevel", [&] {
        bmmo::restart_request_msg msg{};
        msg.content.victim = get_client_id_from_console();
        send(msg);
    });
    console_.register_command("realtime", [&] {
        if (!server_realworld_timestamp_us_) {
            SendIngameMessage("Error: real world time not yet received.", bmmo::ansi::BrightRed);
            return;
        }
        const auto real_timestamp = server_realworld_timestamp_us_ +
                (SteamNetworkingUtils()->GetLocalTimestamp() - server_realworld_timestamp_timestamp_);
        using namespace std::chrono;
        const auto real_time = system_clock::to_time_t(system_clock::time_point{microseconds{real_timestamp}});
        // too much stupidity and compiler shenanigans going on here
        // SendIngameMessage(std::format("Current server-side real-world time: {:%F %T}", floor<seconds>(current_zone()->to_local(real_time))));
        char output[64]; std::tm buf; localtime_s(&buf, &real_time);
        std::strftime(output, sizeof(output), "Current server-side real-world time: %F %T", &buf);
        SendIngameMessage(output);
    });
    console_.register_command("scores", [&] {
        bmmo::map rank_map;
        auto mode = console_.get_next_word(true);
        bool use_local_data = false;
        if (mode == "local") {
            use_local_data = true;
            mode = console_.get_next_word(true);
        }
        if (!console_.empty()) {
            int level = std::clamp(atoi(console_.get_next_word().c_str()), 1, 13);
            bmmo::hex_chars_from_string(rank_map.md5, bmmo::map::original_map_hashes[level]);
            rank_map.level = level;
            rank_map.type = bmmo::map_type::OriginalLevel;
        } else
            rank_map = last_countdown_map_;

        asio::post(thread_pool_, [=, this, rank_map = std::move(rank_map)] {
            bmmo::score_list_msg msg{};
            msg.map = rank_map;
            msg.mode = (mode == "hs") ? bmmo::level_mode::Highscore : bmmo::level_mode::Speedrun;
            if (!use_local_data) {
                msg.serialize();
                send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                return;
            }
            {
                std::lock_guard lk(client_mtx_);
                auto map_it = maps_.find(rank_map.get_hash_bytes_string());
                if (map_it == maps_.end() ||
                        (map_it->second.rankings.first.empty() && map_it->second.rankings.second.empty())) {
                    SendIngameMessage("Error: ranking info not found for the specified map.",
                                      bmmo::ansi::BrightRed);
                    return;
                }
                msg.serialize(map_it->second.rankings);
            }
            receive(msg.raw.str().data(), msg.size());
        });
    });
    console_.register_command("whisper", [&] {
        bmmo::private_chat_msg msg{};
        msg.player_id = get_client_id_from_console();
        msg.chat_content = console_.get_rest_of_line();
        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        SendIngameMessage(std::format("Whispered to {}: {}",
            get_username(msg.player_id), msg.chat_content), bmmo::color_code(msg.code));
    });
    console_.register_aliases("whisper", {"w"});
    console_.register_command("help", [&] { OnAsyncCommand(m_bml, {"mmo"}); });
    console_.register_command("connect", [&] { connect_to_server(console_.get_next_word().c_str()); });
    console_.register_aliases("connect", {"c"});
    console_.register_command("disconnect", [&] { disconnect_from_server(); });
    console_.register_aliases("disconnect", {"d"});
    console_.register_command("list", [&] {
        if (!connected())
            return;

        const auto& cmd_name = console_.get_command_name();
        bool show_id = (cmd_name == "list-id" || cmd_name == "li");

        struct player_data {
            HSteamNetConnection id; std::string name; bool cheated; uint16_t ping;
        };
        std::vector<player_data> players, spectators;
        players.reserve(db_.player_count() + 1);
        db_.for_each([&](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
            if (pair.first == db_.get_client_id())
                return true;
            if (bmmo::name_validator::is_spectator(pair.second.name))
                spectators.emplace_back(pair.first, pair.second.name, pair.second.cheated, pair.second.ping);
            else
                players.emplace_back(pair.first, pair.second.name, pair.second.cheated, pair.second.ping);
            return true;
        });
        if (spectator_mode_)
            spectators.emplace_back(db_.get_client_id(), get_display_nickname(), m_bml->IsCheatEnabled(), get_status().m_nPing);
        else
            players.emplace_back(db_.get_client_id(), get_display_nickname(), m_bml->IsCheatEnabled(), get_status().m_nPing);
        int player_count = players.size();
        std::ranges::sort(players, [](const player_data& i1, const player_data& i2)
            { return boost::algorithm::ilexicographical_compare(i1.name, i2.name); });
        players.insert(players.begin(), spectators.begin(), spectators.end());

        SendIngameMessage(std::format("{} player{} and {} spectator{} ({} total) online:",
                                      player_count, player_count == 1 ? "" : "s",
                                      spectators.size(), spectators.size() == 1 ? "" : "s",
                                      players.size()));
        std::string line; player_count = players.size();
        for (int i = 0; i < player_count; ++i) {
            const auto& [id, name, cheated, ping] = players[i];
            line.append(name + (cheated ? " [CHEAT]" : "") + std::format(" [{}ms]", ping)
                        + (show_id ? std::format(": {}", id) : ""));
            if (i != player_count - 1) line.append(", ");
            if (line.length() > 70) {
                SendIngameMessage(line);
                line.clear();
            }
        }

        if (!line.empty()) SendIngameMessage(line);
    });
    console_.register_aliases("list", {"l", "list-id", "li"});
    console_.register_command("room", [&] {
        if (!connected()) { SendIngameMessage("Error: not connected to a server.", bmmo::ansi::BrightRed); return; }
        const auto sub = console_.get_next_word(true);
        bmmo::room_request_msg msg{};
        // Remembered until the server answers, so its RequestAccepted /
        // RequestDenied can name the subcommand it belongs to.
        pending_room_request pending{};
        if (sub.empty() || sub == "status") { print_room_status(); return; }
        else if (sub == "list") {
            msg.action = bmmo::room::action::List;
            { std::lock_guard lk(room_state_mtx_); room_list_requested_ = true; }
        }
        else if (sub == "create") { msg.action = bmmo::room::action::Create; msg.name = console_.get_rest_of_line(); }
        else if (sub == "join")   {
            msg.action = bmmo::room::action::Join;
            const auto w = console_.get_next_word();
            if (w.empty() || std::atoi(w.c_str()) <= 0) {
                SendIngameMessage("Usage: /mmo room join <id> (\"/mmo room list\" shows the ids).",
                                  bmmo::ansi::BrightRed);
                return;
            }
            msg.room = static_cast<uint32_t>(std::atoi(w.c_str()));
        }
        else if (sub == "leave")  { msg.action = bmmo::room::action::Leave; }
        else if (sub == "ready" || sub == "unready") {
            const auto arg = console_.get_next_word(true);
            bool ready = (sub == "ready");
            if (arg == "off" || arg == "false" || arg == "0") ready = false;
            else if (arg == "on" || arg == "true" || arg == "1") ready = true;
            else if (!arg.empty()) {
                SendIngameMessage(std::format("Error: unknown argument \"{}\". Usage: /mmo room {} [on|off].",
                                  arg, sub), bmmo::ansi::BrightRed);
                return;
            }
            msg.action = ready ? bmmo::room::action::Ready : bmmo::room::action::Unready;
            pending.ready = ready;
        }
        else if (sub == "start") {
            msg.action = bmmo::room::action::Start;
            const auto m = console_.get_next_word(true);
            if (!m.empty() && m != "physics" && m != "shadow") {
                SendIngameMessage(std::format("Error: unknown session mode \"{}\". Usage: /mmo room start [physics|shadow].",
                                  m), bmmo::ansi::BrightRed);
                return;
            }
            msg.mode = pending.mode = (m == "physics") ? bmmo::room::mode::Physics : bmmo::room::mode::Shadow;
        }
        else if (sub == "kick") {
            msg.action = bmmo::room::action::Kick;
            const auto w = console_.get_next_word();
            if (w.empty()) { SendIngameMessage("Usage: /mmo room kick <player|#id>", bmmo::ansi::BrightRed); return; }
            if (w[0] == '#') msg.target = static_cast<HSteamNetConnection>(atoll(w.substr(1).c_str()));
            else msg.target = db_.get_client_id(w);
            if (msg.target == k_HSteamNetConnection_Invalid) { SendIngameMessage(std::format("Error: player \"{}\" not found.", w), bmmo::ansi::BrightRed); return; }
        }
        else if (sub == "close") { msg.action = bmmo::room::action::Close; }
        else if (sub == "session") { SendIngameMessage(physics_session_status_text()); return; }
        else { SendIngameMessage(std::format("Error: unknown room subcommand \"{}\". Try list|create|join|leave|ready|start|kick|close|status|session.", sub), bmmo::ansi::BrightRed); return; }
        pending.action = msg.action;
        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        push_room_request(pending);
    });
    console_.register_command("dnf", [&] {
        if (current_map_.level == 0 || spectator_mode_)
            return;
        send_dnf_message();
    });
    console_.register_command("show", [&] {
        if (console_window_.running()) {
            SendIngameMessage("Error: console is already visible.");
            return;
        }
        /*_dup2(old_stdin, _fileno(stdin));
        _dup2(old_stdout, _fileno(stdout));
        _dup2(old_stderr, _fileno(stderr));*/
        console_window_.show();
    });
    console_.register_command("hide", [&] {
        if (!console_window_.running()) {
            SendIngameMessage("Error: console is already hidden.");
            return;
        }
        console_window_.hide();
    });
    console_.register_command("getpos", [&] {
        if (!connected())
            return;
        std::map<std::string, TimedBallState> states;
        db_.for_each([this, &states](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
            if (pair.first != db_.get_client_id())
                states[pair.second.name] = pair.second.ball_state.front();
            return true;
        });
        states[db_.get_nickname()] = local_state_handler_->get_local_state();
        for (const auto& i: states) {
            SendIngameMessage(std::format("{} is at {:.2f}, {:.2f}, {:.2f} with {} ball.",
                              i.first,
                              i.second.position.x, i.second.position.y, i.second.position.z,
                              i.second.get_type_name()
            ));
        }
    });
    console_.register_aliases("getpos", {"gp"});
    console_.register_command("getmap", [&] {
        if (!connected())
            return;
        db_.for_each([&](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
            if (pair.first == db_.get_client_id())
                return true;
            SendIngameMessage(std::format("{}{} is at the {}{} sector of {}.",
                              pair.second.cheated ? "[CHEAT] " : "",
                              pair.second.name, pair.second.current_sector,
                              bmmo::string_utils::get_ordinal_suffix(pair.second.current_sector),
                              get_map_name(pair.second.current_map)));
            return true;
        });
    });
    console_.register_aliases("getmap", {"gm"});
    console_.register_command("announcemap", [&] {
        if (!connected())
            return;
        send_current_map(bmmo::current_map_state::Announcement);
    });
    console_.register_aliases("announcemap", {"am"});
    console_.register_command("reload", [&] {
        if (!connected() || !m_bml->IsIngame())
            return;
        objects_.reload();
        SendIngameMessage("Reload completed.");
    });
    console_.register_aliases("reload", {"rl"});
    console_.register_command("gettimestamp", [&] {
        db_.for_each([this](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
            SendIngameMessage(std::format("{}: last recalibrated timestamp: {}; mean time diff: {:.4f} s.",
                              pair.second.name,
                              pair.second.ball_state.front().timestamp, pair.second.time_diff / 1e6l));
            return true;
        });
    });
    console_.register_command("countdown", [&] {
        if (!connected() || !m_bml->IsIngame())
            return;
        asio::post(thread_pool_, [this, type = console_.empty() ? -1 : console_.get_next_int()]() {
            if (type != -1) {
                send_countdown_message(static_cast<bmmo::countdown_type>(std::clamp(type, 0, 255)), countdown_mode_);
                return;
            }
            for (int i = 3; i >= 0; --i) {
                send_countdown_message(static_cast<bmmo::countdown_type>(i), countdown_mode_);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
    });
    console_.register_command("ready", [&] {
        if (!connected() || !m_bml->IsIngame() || spectator_mode_)
            return;
        bmmo::player_ready_msg msg{.content = {.ready = (console_.get_command_name() == "ready")}};
        send(msg, k_nSteamNetworkingSend_Reliable);
    });
    console_.register_aliases("ready", {"ready-cancel"});
    console_.register_command("hs", [&] {
        OnAsyncCommand(m_bml, { "mmo", "mode", console_.get_command_name()});
    });
    console_.register_aliases("hs", {"sr"});
    console_.register_command("uuid", [&] {
        SendIngameMessage(std::format("Your UUID: {} (open ModLoader\\ModLoader.log to copy).",
                                      boost::uuids::to_string(config_manager_.get_uuid())));
        SendIngameMessage("Keep this secret as it is possible to impersonate you with your UUID!");
    });
    console_.register_command("fatalerror", [] { std::thread([] { trigger_fatal_error(); }).detach(); });
    console_.register_command("cheat", [&] {
        bmmo::cheat_toggle_msg msg{};
        msg.content.cheated = (console_.get_next_word(true) == "on");
        send(msg, k_nSteamNetworkingSend_Reliable);
    });
    console_.register_command("forcenextrestart", [&] {
        force_next_restart_ = !force_next_restart_;
        if (force_next_restart_)
            SendIngameMessage("ALL players will be restarted with ALL rankings cleared after the next \"Go!\".", bmmo::ansi::WhiteInverse);
        else
            SendIngameMessage("Cleared forced restart status.", bmmo::ansi::WhiteInverse);
    });
    console_.register_command("teleport", [&] {
        if (!(connected() && m_bml->IsIngame() && m_bml->IsCheatEnabled()))
            return;
        auto next_word = console_.get_next_word();
        if (next_word.empty()) return;
        std::optional<PlayerState> state;
        if (next_word[0] == '#')
            state = db_.get((HSteamNetConnection) atoll(next_word.substr(1).c_str()));
        else
            state = db_.get_from_nickname(next_word);
        if (!state.has_value()) {
            SendIngameMessage("Error: requested player \"" + next_word + "\" does not exist.",
                    bmmo::ansi::BrightRed);
            return;
        }
        const std::string name = state.value().name;
        const VxVector position = state.value().ball_state.front().position;

        CKMessageManager* mm = m_bml->GetMessageManager();
        CKMessageType ballDeact = mm->AddMessageType("BallNav deactivate");
        mm->SendMessageSingle(ballDeact, m_bml->GetGroupByName("All_Gameplay"));
        mm->SendMessageSingle(ballDeact, m_bml->GetGroupByName("All_Sound"));

        m_bml->AddTimer(CKDWORD(2), [this, position, name]() {
            ExecuteBB::Unphysicalize(get_current_ball());
            get_current_ball()->SetPosition(VT21_REF(position));
            CK3dEntity* camMF = m_bml->Get3dEntityByName("Cam_MF");
            VxMatrix matrix = camMF->GetWorldMatrix();
            m_bml->RestoreIC(camMF, true);
            camMF->SetPosition(VT21_REF(position));
            camMF->SetWorldMatrix(matrix);
            m_dynamicPos->ActivateInput(0);
            m_dynamicPos->Activate();
            m_phyNewBall->ActivateInput(0);
            m_phyNewBall->Activate();
            m_phyNewBall->GetParent()->Activate();
            SendIngameMessage(std::format("Teleported to \"{}\" at ({:.3f}, {:.3f}, {:.3f}).",
                              name, position.x, position.y, position.z), bmmo::ansi::WhiteInverse);
        });
    });
    console_.register_aliases("teleport", {"tp"});
    console_.register_command("custommap", [&] {
        auto next_word = console_.get_rest_of_line();
        if (boost::iequals(next_word, "reset")) { send_current_map(); return; }
        bmmo::map map; srand((uint32_t)time(nullptr));
        for (auto& i : map.md5) i = rand() % std::numeric_limits<uint8_t>::max();
        bmmo::map_names_msg name_msg{};
        name_msg.maps.emplace(map.get_hash_bytes_string(), next_word);
        name_msg.serialize();
        send(name_msg.raw.str().data(), name_msg.size(), k_nSteamNetworkingSend_Reliable);
        send(bmmo::current_map_msg{ .content = {.map = map, .type = bmmo::current_map_state::NameChange} }, k_nSteamNetworkingSend_Reliable);
        SendIngameMessage(std::format("Current map name set to \"{}\".", next_word), bmmo::ansi::WhiteInverse);
    });
    console_.register_command("mode", [&] {
        if (console_.get_next_word(true) == "hs")
            countdown_mode_ = bmmo::level_mode::Highscore;
        else
            countdown_mode_ = bmmo::level_mode::Speedrun;
        std::string mode_name = (countdown_mode_ == bmmo::level_mode::Highscore) ? "Highscore" : "Speedrun";
        SendIngameMessage(std::format("Level mode set to {} for future countdowns.", mode_name),
                          bmmo::ansi::WhiteInverse);
        if (!connected()) {
            current_level_mode_ = countdown_mode_;
            SendIngameMessage(std::format("Local level mode set to {}.", mode_name));
        }
    });
    console_.register_command("schedule", [&] {
        static std::set<std::string> scheduled_commands;
        auto next_word = console_.get_next_word(true);
        if (next_word == "add") {
            auto timeout = atof(console_.get_next_word().c_str());
            if (console_.empty())
                return;
            auto cmd = console_.get_rest_of_line();
            if (cmd.starts_with("/"))
                cmd.erase(0, 1);
            if (cmd.starts_with("mmo ") || cmd.starts_with("ballancemmo "))
                cmd.erase(0, cmd.find(' ') + 1);
            cmd = std::format("{:g} {}", timeout, cmd);
            scheduled_commands.insert(cmd);
            SendIngameMessage(std::format("Scheduled command \"{}\" successfully.", cmd));
            m_bml->AddTimer(float(timeout * 1000), [this, cmd = std::move(cmd)] {
                const auto it = scheduled_commands.find(cmd);
                if (it == scheduled_commands.end())
                    return;
                SendIngameMessage(std::format("Executing scheduled command: {}", cmd));
                OnFullCommand(cmd.substr(cmd.find(' ') + 1));
                scheduled_commands.erase(it);
            });
        }
        else if (next_word == "list") {
            int i = 0;
            for (const auto& cmd : scheduled_commands) {
                ++i;
                SendIngameMessage(std::format("[{}] {}", i, cmd));
            }
        }
        else if (next_word == "delete") {
            auto index = std::atoi(console_.get_next_word().c_str());
            if (index <= 0 || index > (int) scheduled_commands.size()) {
                SendIngameMessage("Error: invalid index.");
                return;
            }
            scheduled_commands.erase(std::next(scheduled_commands.begin(), index - 1));
            SendIngameMessage(std::format("Deleted schedule at position {} successfully.", index));
        }
        else if (next_word == "clear") {
            scheduled_commands.clear();
            SendIngameMessage("Cleared all scheduled commands.");
        }
        else {
            SendIngameMessage("schedule add <timeout> <cmd>: add a command to the scheduler.");
            SendIngameMessage("schedule list: list all currently scheduled commands.");
            SendIngameMessage("schedule delete <index>: delete a scheduled command by its index (from schedule list).");
            SendIngameMessage("schedule clear: remove all scheduled commands.");
        }
    });
#ifdef BMMO_WITH_PLAYER_SPECTATION
    console_.register_command("spectate", [&] {
        HSteamNetConnection id = k_HSteamNetConnection_Invalid;
        std::string extra_info;
        if (console_.get_command_name() == "rankspectate") {
            int rank = std::atoi(console_.get_next_word().c_str());
            std::lock_guard lk(player_status_list_mtx_);
            if (rank > 0 && rank <= player_status_list_.size()) {
                std::string current_map_name = current_map_.get_display_name();
                int counter = 0;
                for (const auto& i : player_status_list_) {
                    if (i.map_name != current_map_name)
                        continue;
                    ++counter;
                    if (counter == rank) {
                        const auto& name = i.name;
                        id = (name == get_display_nickname()) ? db_.get_client_id() : db_.get_client_id(name);
                        extra_info = std::format("#{} in {}", rank, current_map_name);
                        break;
                    }
                }
            }
        }
        else {
            auto next_word = console_.get_next_word();
            if (next_word.starts_with('##') && next_word.length() > 2) {
                int index = std::atoi(next_word.substr(2).c_str());
                if (index >= spect_bindings_.size())
                    index = 0;
                next_word = spect_bindings_[index];
                extra_info = std::format("bound as #{}", index);
            }
            if (next_word.starts_with('#'))
                id = (HSteamNetConnection)atoll(next_word.substr(1).c_str());
            else
                id = (next_word == get_display_nickname()) ? db_.get_client_id() : db_.get_client_id(next_word);
        }
        if (id == objects_.get_spectated_id()) {
            if (id == k_HSteamNetConnection_Invalid)
                SendIngameMessage("Already not spectating.");
            else
                SendIngameMessage("Already spectating the same player.");
            return;
        }
        objects_.set_spectated_id(id);
        if (id == k_HSteamNetConnection_Invalid) {
            m_bml->GetRenderContext()->AttachViewpointToCamera(last_cam_ ? last_cam_ : m_bml->GetTargetCameraByName("InGameCam"));
            spectating_first_person_ = false;
            m_bml->GetGroupByName("HUD_sprites")->Show();
            m_bml->GetGroupByName("LifeBalls")->Show();
            if (spectator_label_) spectator_label_->update("[Spectator Mode]");
            SendIngameMessage("Exited spectation.");
            return;
        }
        const auto& state = db_.get(id);
        if (!state.has_value()) {
            SendIngameMessage("Error: player not found.");
            return;
        }
        VxVector spect_cam_pos;
        spect_cam_->GetPosition(&spect_cam_pos);
        CKCamera* cam = m_bml->GetTargetCameraByName("InGameCam");
        spect_cam_->SetWorldMatrix(cam->GetWorldMatrix());
        int width, height;
        cam->GetAspectRatio(width, height);
        spect_cam_->SetAspectRatio(width, height);
        spect_cam_->SetFov(cam->GetFov());
        VxVector cam_pos, ball_pos;
        cam->GetPosition(&cam_pos);
        player_ball_->GetPosition(&ball_pos);
        spect_pos_diff_ = cam_pos - ball_pos;
        if (auto attached = m_bml->GetRenderContext()->GetAttachedCamera(); attached != spect_cam_)
            last_cam_ = attached; // save last camera if we weren't spectating
        spect_cam_->SetPosition(VT21_REF(spect_cam_pos));
        m_bml->GetRenderContext()->AttachViewpointToCamera(spect_cam_);
        spectating_first_person_ = true;
        spect_target_pos_ = objects_.get_ball_pos(objects_.get_spectated_id());
        m_bml->GetGroupByName("HUD_sprites")->Show(CKHIDE);
        m_bml->GetGroupByName("LifeBalls")->Show(CKHIDE);
        if (spectator_label_) spectator_label_->update(std::format("[Spectating: {}]", state.value().name));
        objects_.update(SteamNetworkingUtils()->GetLocalTimestamp(), true); // update ping info
        SendIngameMessage(std::format("Spectating {}{}.", state.value().name,
                          extra_info.empty() ? "" : std::format(" [{}]", extra_info)));
    });
    console_.register_aliases("spectate", {"rankspectate"});
    console_.register_command("bindspectation", [&] {
        spect_bindings_ = decltype(spect_bindings_){"#0"};
        while (!console_.empty())
            spect_bindings_.emplace_back(console_.get_next_word(true));
        std::string names_text;
        for (int i = 1; i < spect_bindings_.size(); ++i) {
            names_text.append(std::format("[{}] {}, ", i, spect_bindings_[i]));
        }
        names_text.erase(names_text.length() - std::strlen(", "));
        SendIngameMessage("Spectator hotkeys bound to " + names_text + ".");
        SendIngameMessage("You can either press RightAlt + , (or .) + number");
        SendIngameMessage("Or run \"/mmo spectate ##number\" to activate spectation.");
    });
#endif
}

std::vector<std::string> BallanceMMOClient::OnTabComplete(IBML* bml, const std::vector<std::string>& args) {
    const size_t length = args.size();
    std::string lower1;
    if (length > 1) lower1 = boost::algorithm::to_lower_copy(args[1]);

    switch (length) {
        case 1:
            break;
        case 2: {
            return console_.get_command_list();
        }
        case 3:
        default: {
            if (std::set<std::string>{
                "teleport", "tp", "kick", "crash", "restartlevel",
                "whisper", "w", "say", "s",
                "announce", "a", "b", "bulletin", "notice",
#ifdef BMMO_WITH_PLAYER_SPECTATION
                "spectate", "bindspectation",
#endif
            }.contains(lower1)) {
                std::vector<std::string> options;
                options.reserve(db_.player_count() + 1);
                bool hint_client_id = (args[args.size() - 1]).starts_with('#');
                db_.for_each([=, this, &options](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
                    if (pair.first == db_.get_client_id()) return true;
                    if (hint_client_id)
                        options.push_back('#' + std::to_string(pair.first));
                    else
                        options.push_back(pair.second.name);
                    return true;
                });
                if (hint_client_id)
                    options.push_back('#' + std::to_string(db_.get_client_id()));
                else
                    options.push_back(get_display_nickname());
                return options;
            }
            else if (lower1 == "room") {
                if (length == 3)
                    return {"list", "create", "join", "leave", "ready", "unready", "start", "kick", "close", "status"};
                const auto sub = boost::algorithm::to_lower_copy(args[2]);
                if (length == 4 && sub == "start") return {"shadow", "physics"};
                if (length == 4 && (sub == "ready" || sub == "unready")) return {"on", "off"};
                if (length == 4 && sub == "kick") {
                    std::vector<std::string> options;
                    db_.for_each([&](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
                        if (pair.first != db_.get_client_id()) options.push_back(pair.second.name);
                        return true;
                    });
                    return options;
                }
            }
            else if (lower1 == "mode")
                return {"hs", "sr"};
            else if (lower1 == "scores")
                return {"hs", "sr", "local"};
            else if (lower1 == "schedule") {
                if (length == 3)
                    return {"add", "list", "delete", "clear"};
                if (length > 4)
                    return OnTabComplete(bml, std::vector<std::string>(args.begin() + 3, args.end()));
            }
            break;
        }
    }
    return {};
}

void BallanceMMOClient::OnTrafo(int from, int to)
{
    local_state_handler_->poll_and_send_state_forced(player_ball_);
    //throw std::runtime_error("On trafo");
}

void BallanceMMOClient::OnPeerTrafo(uint64_t id, int from, int to)
{
    logger_->Info("OnPeerTrafo, %d -> %d", from, to);
    /*PeerState& peer = peer_[id];
    peer.current_ball = to;
    peer.balls[from]->Show(CKHIDE);
    peer.balls[to]->Show(CKSHOW);*/
}

void BallanceMMOClient::terminate(long delay) {
    get_instance()->SendIngameMessage(
        std::format("Nuking process in {} seconds...", delay).c_str());
    std::this_thread::sleep_for(std::chrono::seconds(delay));

    std::terminate();
}

void BallanceMMOClient::connect_to_server(const char* address, const char* name) {
    bool canceled = false;
    notify_listeners([&](const auto& i) { canceled |= !i->on_pre_login(address, name); });
    if (canceled) return;

    if (std::strcmp(server_addr_.c_str(), address) == 0) {
        if (connected()) {
            std::lock_guard lk(bml_mtx_);
            SendIngameMessage("Already connected.");
            return;
        }
        else if (connecting()) {
            std::lock_guard lk(bml_mtx_);
            SendIngameMessage("Connecting in process, please wait...");
            return;
        }
    }
    if (std::strlen(address) == 0) {
        utils_.schedule_sync_call([this] { server_list_->enter_gui(); });
        return;
    }
    if (connected() || connecting()) {
        disconnect_from_server();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    SendIngameMessage("Resolving server address...");
    resolving_endpoint_ = true;
    // Bootstrap io_context
    work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(io_ctx_.get_executor());
    asio::post(thread_pool_, [this]() {
        if (io_ctx_.stopped())
            io_ctx_.reset();
        io_ctx_.run();
    });

    // Resolve address
    server_addr_ = address;
    server_name_ = (std::strlen(name) == 0) ? address : name;
    const auto& [host, port] = bmmo::hostname_parser(server_addr_).get_host_components();
    logger_->Info("Server name: %s; address: %s (%s:%s).",
                      server_name_.c_str(), server_addr_.c_str(), host.c_str(), port.c_str());
    resolver_ = std::make_unique<asio::ip::udp::resolver>(io_ctx_);
    resolver_->async_resolve(host, port, [this](asio::error_code ec, asio::ip::udp::resolver::results_type results) {
        resolving_endpoint_ = false;
        std::lock_guard lk(bml_mtx_);
        // If address correctly resolved...
        if (!ec) {
            SendIngameMessage("Server address resolved.");

            for (const auto& i : results) {
                auto endpoint = i.endpoint();
                std::string connection_string = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
                logger_->Info("Trying %s", connection_string.c_str());
                if (connect(connection_string)) {
                    SendIngameMessage("Connecting...");
                    if (network_thread_.joinable())
                        network_thread_.join();
                    network_thread_ = utils::create_named_thread(L"BMMO_Networking", [this]() { run(); });
                    work_guard_.reset();
                    io_ctx_.stop();
                    resolver_.reset();
                    return;
                }
            }
            // but none of the resolve results are unreachable...
            SendIngameMessage("Failed to connect to server. All resolved address appears to be unresolvable.");
            work_guard_.reset();
            io_ctx_.stop();
            resolver_.reset();
            return;
        }
        // If not correctly resolved...
        SendIngameMessage("Failed to resolve hostname.");
        logger_->Error(ec.message().c_str());
        work_guard_.reset();
        io_ctx_.stop();
    });
}

void BallanceMMOClient::disconnect_from_server() {
    if (!connecting() && !connected()) {
        std::lock_guard lk(bml_mtx_);
        SendIngameMessage("Already disconnected.");
    }
    else {
        //client_.disconnect();
        //ping_->update("");
        //status_->update("Disconnected");
        //status_->paint(0xffff0000);
        cleanup();
        SendIngameMessage("Disconnected.");

        clear_ping_text();
        set_status_text("Disconnected", 0xffff0000);
    }
}

void BallanceMMOClient::reconnect(int delay, float scale) {
    asio::post(thread_pool_, [this, delay, scale]() mutable {
        if (reconnection_count_ >= 3) {
            SendIngameMessage("Failed to connect to the server after 3 attempts. Server will not be reconnected.");
            reconnection_count_ = 0;
            return;
        }
        delay = int(delay * std::powf(scale, static_cast<float>(reconnection_count_)));
        ++reconnection_count_;
        SendIngameMessage(std::format("Attempting to reconnect to [{}] in {} second{} ...",
                                      server_name_, delay, delay == 1 ? "" : "s"));
        std::this_thread::sleep_for(std::chrono::seconds(delay));
        if (!connecting() && !connected())
            connect_to_server(server_addr_.c_str(), server_name_.c_str());
    });
}

void BallanceMMOClient::on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
    logger_->Info("Connection status changed. %d -> %d", pInfo->m_eOldState, pInfo->m_info.m_eState);
    estate_ = pInfo->m_info.m_eState;

    switch (pInfo->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_None:
        clear_ping_text();
        set_status_text("Disconnected", 0xffff0000);
        break;
    case k_ESteamNetworkingConnectionState_ClosedByPeer: {
        std::string s = std::format("Reason: {} ({})", pInfo->m_info.m_szEndDebug, pInfo->m_info.m_eEndReason);
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
            // Note: we could distinguish between a timeout, a rejected connection,
            // or some other transport problem.
            SendIngameMessage("Connect failed. (ClosedByPeer)");
            logger_->Warn(pInfo->m_info.m_szEndDebug);
            break;
        }
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected) {
            SendIngameMessage("You've been disconnected from the server.");
        }
        SendIngameMessage(s.c_str());
        cleanup();
        const int nReason = pInfo->m_info.m_eEndReason;
        if (nReason >= bmmo::connection_end::Crash && nReason <= bmmo::connection_end::PlayerKicked_Max)
            terminate(5);
        else if (nReason >= bmmo::connection_end::AutoReconnection_Min && nReason < bmmo::connection_end::AutoReconnection_Max) {
            // auto reconnect
            reconnect(nReason - bmmo::connection_end::AutoReconnection_Min);
        }
        else if (nReason > k_ESteamNetConnectionEnd_App_Max) {
            reconnect(5, 2.0f);
        }
        break;
    }
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
        std::string s = std::format("Reason: {} ({})", pInfo->m_info.m_szEndDebug, pInfo->m_info.m_eEndReason);
        clear_ping_text();
        set_status_text("Disconnected", 0xffff0000);
        // Print an appropriate message
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
            // Note: we could distinguish between a timeout, a rejected connection,
            // or some other transport problem.
            SendIngameMessage("Connect failed. (ProblemDetectedLocally)");
            logger_->Warn(pInfo->m_info.m_szEndDebug);
        }
        else {
            // NOTE: We could check the reason code for a normal disconnection
            SendIngameMessage("Connect failed. (UnknownError)");
            logger_->Warn("Unknown error. (%d->%d) %s", pInfo->m_eOldState, pInfo->m_info.m_eState, pInfo->m_info.m_szEndDebug);
        }
        SendIngameMessage(s.c_str());
        // Clean up the connection.  This is important!
        // The connection is "closed" in the network sense, but
        // it has not been destroyed.  We must close it on our end, too
        // to finish up.  The reason information do not matter in this case,
        // and we cannot linger because it's already closed on the other end,
        // so we just pass 0's.

        //interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
        cleanup();

        if (pInfo->m_info.m_eEndReason > k_ESteamNetConnectionEnd_App_Max) {
            reconnect(5, 2.0f);
        }
        break;
    }

    case k_ESteamNetworkingConnectionState_Connecting:
        // We will get this callback when we start connecting.
        set_status_text("Connecting", 0xFFF6A71B);
        break;

    case k_ESteamNetworkingConnectionState_Connected: {
        std::lock_guard lk(bml_mtx_);
        set_status_text("Connected (Login requested)");
        SendIngameMessage("Connected to server.");
        std::string nickname = db_.get_nickname();
        // check_and_save_name_change_time() writes the external config file and
        // read-modify-writes config_manager_'s name-change bookkeeping, which
        // the game thread also touches; keep it off the network thread.
        utils_.run_on_game_thread([this] { config_manager_.check_and_save_name_change_time(); });
        // spectator_mode_ mirrors a BML property; the game thread refreshes it
        // every frame in OnProcess, so just read it here
        if (spectator_mode_) {
            nickname = bmmo::name_validator::get_spectator_nickname(nickname);
            SendIngameMessage("Note: Spectator Mode is enabled. Your actions will be invisible to other players.");
            local_state_handler_ = std::make_unique<spectator_state_handler>(thread_pool_, this, GetLogger());
        }
        else {
            local_state_handler_ = std::make_unique<player_state_handler>(thread_pool_, this, GetLogger());
        }
        // sprites and the current ball are engine objects: creating or reading
        // them here would race the renderer, so hand that part to the game thread
        utils_.run_on_game_thread([this] {
            if (spectator_mode_) {
                spectator_label_ = std::make_shared<text_sprite>("Spectator_Label", "[Spectator Mode]", RIGHT_MOST, 0.9625f);
                spectator_label_->sprite_->SetFont("Arial", utils_.get_display_font_size(11.72f), 500, false, false);
                spectator_label_->set_visible(true);
            }
            player_ball_ = get_current_ball();
            if (player_ball_ && local_state_handler_)
                local_state_handler_->set_ball_type(db_.get_ball_id(player_ball_->GetName()));
        });

        SendIngameMessage(("Logging in as \"" + nickname + "\"...").c_str());
        bmmo::login_request_v3_msg msg{};
        msg.nickname = nickname;
        msg.version = bmmo::current_version;
        // the mirror, not IsCheatEnabled(): this runs on the network thread
        msg.cheated = own_cheat_enabled_.load() && !spectator_mode_; // always false in spectator mode
        memcpy(msg.uuid, &(config_manager_.get_uuid()), sizeof(config_manager_.get_uuid()));
        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        if (ping_thread_.joinable())
            ping_thread_.join();
        ping_thread_ = utils::create_named_thread(L"BMMO_Ping", [this]() {
            { // race condition mitigation
                std::unique_lock client_lk(client_mtx_);
                client_cv_.wait(client_lk);
            }
            average_ping_ = (float) get_ping();
            while (connected()) {
                auto status = get_status();
                average_ping_ = (average_ping_ * 3 + status.m_nPing) / 4;
                // publish the text; the ping sprite itself is only ever
                // touched on the game thread, in OnProcess
                {
                    std::lock_guard lk(ping_text_mtx_);
                    ping_text_ = utils::pretty_status(status);
                    ping_text_pending_ = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            };
        });
        break;
    }
    default:
        // Silences -Wswitch
        break;
    }
}

// Game thread. The countdown cue, shared by the countdown_msg handler and the
// physics session lead-in (session/physics_session_client.cpp).
void BallanceMMOClient::play_countdown_sound(bmmo::countdown_type type) {
    using ct = bmmo::countdown_type;
    switch (type) {
        case ct::Unknown:
            return;
        case ct::Go:
            play_wave_sound(sound_go_, true);
            return;
        case ct::Ready:
        case ct::ConfirmReady: {
            // one timer per note: CKWaveSound::SetPitch is an engine call, and
            // the old pool task slept between the notes
            static constexpr float base = 0.4375f;// 1.29f;
            float delay = 0.0f;
            for (const auto pitch: {base, base * std::powf(2.0f, 3.0f / 12), base * std::powf(2.0f, 7.0f / 12), base * 2.0f}) {
                m_bml->AddTimer(delay, [this, pitch] {
                    if (!sound_countdown_) return;
                    sound_countdown_->SetPitch(pitch);
                    play_wave_sound(sound_countdown_, true);
                });
                delay += 500.0f;
            }
            return;
        }
        default:  // one dong per number
            if (!sound_countdown_) return;
            sound_countdown_->SetPitch(0.875f);
            play_wave_sound(sound_countdown_, true);
            return;
    }
}

void BallanceMMOClient::on_message(ISteamNetworkingMessage* network_msg) {
    // general_message is a max-sized view used only to read the opcode, so it
    // is checked against sizeof(opcode) rather than its own (huge) size
    if (network_msg->m_cbSize < static_cast<decltype(network_msg->m_cbSize)>(sizeof(bmmo::opcode))) {
        logger_->Error("Invalid message with size %d received.", network_msg->m_cbSize);
        return;
    }
    auto* raw_msg = reinterpret_cast<bmmo::general_message*>(network_msg->m_pData);

    switch (raw_msg->code) {
    case bmmo::OwnedBallState: {
        auto* obs = bmmo::message_utils::view_as<bmmo::owned_ball_state_msg>(network_msg);
        if (!obs) break;
        bool success = db_.update(obs->content.player_id, TimedBallState(obs->content.state));
        //assert(success);
        if (!success) {
            logger_->Warn("Update db failed: Cannot find such ConnectionID %u. (on_message - OwnedBallState)", obs->content.player_id);
        }
        /*auto state = db_.get(obs->content.player_id);
        logger_->Info("%s: %d, (%.2lf, %.2lf, %.2lf), (%.2lf, %.2lf, %.2lf, %.2lf)",
            state->name.c_str(),
            state->ball_state.type,
            state->ball_state.position.x,
            state->ball_state.position.y,
            state->ball_state.position.z,
            state->ball_state.rotation.x,
            state->ball_state.rotation.y,
            state->ball_state.rotation.z,
            state->ball_state.rotation.w);*/
        break;
    }
    case bmmo::OwnedBallStateV2: {
        bmmo::owned_ball_state_v2_msg msg;
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        for (auto& i : msg.balls) {
            if (!db_.update(i.player_id, TimedBallState(i.state)) && i.player_id != db_.get_client_id()) {
                logger_->Warn("Update db failed: Cannot find such ConnectionID %u. (on_message - OwnedBallState)", i.player_id);
            }
        }
        break;
    }
    case bmmo::OwnedTimedBallState:
        break;
    case bmmo::OwnedCompressedBallState: {
        bmmo::owned_compressed_ball_state_msg msg;
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        std::unique_lock bml_lk(bml_mtx_, std::try_to_lock);
        if (!bml_lk)
            break;
        for (const auto& i : msg.balls) {
            //static char t[128];
            /*snprintf(t, 128, "%u: %d, (%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f, %.2f), %lld",
                              i.player_id,
                              i.state.type,
                              i.state.position.x, i.state.position.y, i.state.position.z,
                              i.state.rotation.x, i.state.rotation.y, i.state.rotation.z, i.state.rotation.w,
                              int64_t(i.state.timestamp));*/
            if (!db_.update(i.player_id, TimedBallState(i.state)) && i.player_id != db_.get_client_id()) {
                logger_->Warn("Update db failed: Cannot find such ConnectionID %u. (on_message - OwnedBallState)", i.player_id);
            }
        }
        for (const auto& i : msg.unchanged_balls) {
            if (!db_.update(i.player_id, i.timestamp) && i.player_id != db_.get_client_id()) {
                logger_->Warn("Update db failed: Cannot find such ConnectionID %u. (on_message - OwnedBallState)", i.player_id);
            }
        }
        break;
    }
    case bmmo::LoginAcceptedV2:
    case bmmo::LoginAccepted: {
        /*status_->update("Connected");
        status_->paint(0xff00ff00);
        SendIngameMessage("Logged in.");
        bmmo::login_accepted_msg msg;
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (!msg.deserialize()) {
            logger_->Error("Deserialize failed!");
        }
        logger_->Info("Online players: ");

        for (auto& i : msg.online_players) {
            if (i.second == db_.get_nickname()) {
                db_.set_client_id(i.first);
            } else {
                db_.create(i.first, i.second);
            }
            logger_->Info(i.second.c_str());
        }*/
        logger_->Warn("Outdated LoginAccepted%s msg received!", (raw_msg->code == bmmo::LoginAcceptedV2) ? "V2" : "");
        break;
    }
    case bmmo::LoginAcceptedV3: {
        auto msg = bmmo::message_utils::deserialize<bmmo::login_accepted_v3_msg>(network_msg);
        std::lock_guard lk(bml_mtx_);

        if (logged_in_) {
            logger_->Info("New LoginAccepted message received. Resetting current data.");
#ifdef BMMO_WITH_PLAYER_SPECTATION
            if (objects_.get_spectated_id() != k_HSteamNetConnection_Invalid)
                OnFullCommand("spectate"); // exit spectation properly
#endif // BMMO_WITH_PLAYER_SPECTATION
            db_.clear();
            // destroying the spirit balls destroys CK objects
            utils_.run_on_game_thread([this] { objects_.destroy_all_objects(); });
        }
        logger_->Info("%d player(s) online: ", msg.online_players.size());
        auto nickname = get_display_nickname();
        for (const auto& [id, data] : msg.online_players) {
            if (data.name == nickname) {
                db_.set_client_id(id);
            } else {
                db_.create(id, data.name, data.cheated);
                int64_t timestamp = db_.get_timestamp_ms();
                db_.update_map(id, data.map, data.sector, timestamp);
                maps_.try_emplace(data.map.get_hash_bytes_string());
                if (!bmmo::name_validator::is_spectator(data.name))
                    update_sector_timestamp(data.map, data.sector, timestamp); // recursive locking of bml_mtx_
            }
            logger_->Info(data.name.c_str());
        }

        if (logged_in_) {
            asio::post(thread_pool_, [this] {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                db_.reset_time_data();
            });
            break;
        }
        logged_in_ = true;
        client_cv_.notify_all();
        reconnection_count_ = 0;
        SendIngameMessage("Logged in.", bmmo::color_code(msg.code));

        auto t = std::time(nullptr);
        char utc_time_str[32];
        std::strftime(utc_time_str, sizeof(utc_time_str), "%Y-%m-%d %H:%M:%S", std::gmtime(&t));
        SendIngameMessage(std::format("BMMO version: {} at {}; local time: {} UTC",
            bmmo::current_version.to_string(), bmmo::string_utils::get_build_time_string(), utc_time_str
        ).c_str());

        // The rest of the login sequence reads the status sprite, the current
        // ball and the mod list - all engine state. Hand it to the game thread
        // and send the resulting messages from there.
        utils_.run_on_game_thread([this] {
            set_status_text("Connected", 0xff00ff00);

            // post-connection actions
            player_ball_ = get_current_ball();
            if (m_bml->IsIngame() && player_ball_ != nullptr && local_state_handler_)
                local_state_handler_->poll_and_send_state_forced(player_ball_);
            if (!current_map_.name.empty()) {
                const auto hash = current_map_.get_hash_bytes_string();
                if (!has_map_name(hash)) {
                    send_current_map_name();
                    add_map_name(hash, current_map_.name);
                } else
                    current_map_.name = get_raw_map_name(hash);
            }
            send_current_map();

            notify_listeners([this](const auto& i) { i->on_login(server_addr_.c_str(), server_name_.c_str()); });

            if (config_manager_.get_player_list_visible())
                show_player_list();

            if (spectator_mode_)
                return;

            const auto count = m_bml->GetModCount();
            bmmo::mod_list_msg mod_msg{};
            for (auto i = 0; i < count; ++i) {
                auto* mod = m_bml->GetMod(i);
                if (mod == this) continue; // ignore bmmo itself
                std::string mod_id = bmmo::string_utils::ansi_to_utf8(mod->GetID());
#ifdef BMMO_USE_BML_PLUS
                if (mod_id == "BML") mod_id = "BML+"; // rename BML to BML+ for mod list
#endif // BMMO_USE_BML_PLUS
                mod_msg.mods.try_emplace(mod_id, bmmo::string_utils::ansi_to_utf8(mod->GetVersion()));
            }
            mod_msg.serialize();
            send(mod_msg.raw.str().data(), mod_msg.size(), k_nSteamNetworkingSend_Reliable);

            bmmo::hash_data_msg hash_msg{};
            hash_msg.data = md5_data_;
            hash_msg.serialize();
            send(hash_msg.raw.str().data(), hash_msg.size(), k_nSteamNetworkingSend_Reliable);
        });
        break;
    }
    case bmmo::PlayerConnected: {
        //bmmo::player_connected_msg msg;
        //msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        //msg.deserialize();
        //SendIngameMessage((msg.name + " joined the game.").c_str());
        //if (m_bml->IsIngame()) {
        //    logger_->Info("Creating game objects for %u, %s", msg.connection_id, msg.name.c_str());
        //    objects_.init_player(msg.connection_id, msg.name);
        //}

        //logger_->Info("Creating state entry for %u, %s", msg.connection_id, msg.name.c_str());
        //db_.create(msg.connection_id, msg.name);

        //// TODO: call this when the player enters a map
        logger_->Warn("Outdated PlayerConnected msg received!");

        break;
    }
    case bmmo::PlayerConnectedV2: {
        std::lock_guard lk(bml_mtx_);
        bmmo::player_connected_v2_msg msg;
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        SendIngameMessage(std::format("{} joined the game with cheat [{}].", msg.name, msg.cheated ? "on" : "off"),
                          bmmo::color_code(msg.code));
        //if (m_bml->IsIngame()) {
        //    logger_->Info("Creating game objects for %u, %s", msg.connection_id, msg.name.c_str());
        //    objects_.init_player(msg.connection_id, msg.name, msg.cheated);
        //}
        // don't init objects outside of main thread; we'll use objects_.update() to auto-init them

        logger_->Info("Creating state entry for %u, %s", msg.connection_id, msg.name.c_str());
        db_.create(msg.connection_id, msg.name, msg.cheated);

        play_wave_sound(sound_bubble_);
        utils_.flash_window();

        // other mods' callbacks will call BML APIs: run them on the game thread
        utils_.run_on_game_thread([this, id = msg.connection_id] {
            notify_listeners([id](const auto& i) { i->on_player_login(id); });
        });
        // TODO: call this when the player enters a map

        break;
    }
    case bmmo::PlayerDisconnected: {
        std::lock_guard lk(bml_mtx_);
        auto* msg = reinterpret_cast<bmmo::player_disconnected_msg *>(network_msg->m_pData);
        auto state = db_.get(msg->content.connection_id);
        //assert(state.has_value());
        if (state.has_value()) {
            SendIngameMessage(state->name + " left the game.", bmmo::color_code(msg->code));
            play_wave_sound(sound_knock_);
            utils_.flash_window();
            db_.remove(msg->content.connection_id);
            utils_.schedule_sync_call([this, id = msg->content.connection_id] {
                objects_.remove(id);
                notify_listeners([id](const auto& i) { i->on_player_logout(id); });
            });
        }
        break;
    }
    case bmmo::Chat: {
        bmmo::chat_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        SendIngameMessage(std::format("{}: {}", get_username(msg.player_id), msg.chat_content).c_str());
        utils_.flash_window();
        break;
    }
    case bmmo::PrivateChat: {
        bmmo::private_chat_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        SendIngameMessage(std::format("{} whispers to you: {}",
                                      get_username(msg.player_id), msg.chat_content), bmmo::color_code(msg.code));
        utils_.flash_window();
        break;
    }
    case bmmo::ImportantNotification: {
        using in_msg = bmmo::important_notification_msg; in_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        std::string name = msg.type >= in_msg::PLAIN_MSG_SHIFT ? "" : get_username(msg.player_id);
        SendIngameMessage(std::format("[{}] {}: {}", msg.get_type_name(), name,
#ifdef BMMO_USE_BML_PLUS
                          (loader_version_ >= BMLVersion{ 0, 3, 9 }) ?
                          bmmo::string_utils::parse_line_breaks(msg.chat_content, " ↳ ") :
#endif // BMMO_USE_BML_PLUS
                          msg.chat_content), msg.get_ansi_color());
        asio::post(thread_pool_, [this, name, msg = std::move(msg)]() mutable {
            utils_.flash_window();
            play_wave_sound(sound_notification_, !utils_.is_foreground_window());
            float font_size = 16.0f, y_pos = 0.684f;
            int font_weight = 400;
            if (msg.type % in_msg::PLAIN_MSG_SHIFT == in_msg::Announcement) {
                font_size = 19.0f;
                font_weight = 700;
                y_pos = 0.4f;
            }
            auto line_count = utils_.split_lines(msg.chat_content, 0.7f, font_size, font_weight);
            if (msg.type % in_msg::PLAIN_MSG_SHIFT == in_msg::Announcement) {
                msg.chat_content += '\n';
                ++line_count;
            }
            if (msg.type < in_msg::PLAIN_MSG_SHIFT)
                msg.chat_content += "[" + name + "]";
            utils_.display_important_notification(msg.chat_content, font_size, line_count, font_weight, y_pos);
        });
        break;
    }
    case bmmo::PlayerReady: {
        auto* msg = bmmo::message_utils::view_as<bmmo::player_ready_msg>(network_msg);
        if (!msg) break;
        SendIngameMessage(std::format("{} is{} ready to start ({} player{} ready).",
                          get_username(msg->content.player_id),
                          msg->content.ready ? "" : " not",
                          msg->content.count, msg->content.count == 1 ? "" : "s"));
        break;
    }
    case bmmo::Countdown: {
        auto* msg = bmmo::message_utils::view_as<bmmo::countdown_msg>(network_msg);
        if (!msg) break;

        int time_for_14_frames_ms = 0;
        {
            std::lock_guard lk(frame_times_mtx_);
            if (frame_timestamps_in_last_45s_.size() < 1024) {
                // not enough data - assume 240 fps
                time_for_14_frames_ms = 14 * 1000 / 240;
            }
            else {
                time_for_14_frames_ms = (int)(
                    (frame_timestamps_in_last_45s_.back() - frame_timestamps_in_last_45s_.front()) /
                    ((int64_t)frame_timestamps_in_last_45s_.size() - 1) * 14 / 1000
                );
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, 160 - (int)average_ping_ - time_for_14_frames_ms)));
        std::string sender_name = get_username(msg->content.sender),
                    map_name = get_map_name(msg->content.map);
        last_countdown_map_ = msg->content.force_restart ? current_map_ : msg->content.map;

        notify_listeners([msg](const auto& i) { i->on_countdown(msg); });

        switch (msg->content.type) {
            using ct = bmmo::countdown_type;
            case ct::Unknown:
                return;
            case ct::Go: {
                SendIngameMessage(std::format("[{}]: {}{} - Go!",
                                  sender_name, map_name, msg->content.get_level_mode_label()),
                                  bmmo::color_code(msg->code));
                if (msg->content.map == current_map_ || msg->content.force_restart)
                    current_level_mode_ = msg->content.mode;
                // asio::post(thread_pool_, [this] { play_beep(int(440 * std::powf(2.0f, 5.0f / 12)), 1000); });
                utils_.run_on_game_thread([this] { play_countdown_sound(bmmo::countdown_type::Go); });
                // Everything below drives gameplay: it resets level timers,
                // restarts the level through the engine's behavior graph and
                // writes the energy array. That belongs on the game thread, so
                // copy what it needs (the receive buffer dies with this call)
                // and hand it over.
                utils_.run_on_game_thread([this,
                        force_restart = bool(msg->content.force_restart),
                        restart_level = bool(msg->content.restart_level),
                        map_matches = (msg->content.map == current_map_)] {
                    const auto start_timestamp = m_bml->GetTimeManager()->GetTime();
                    if (force_restart) {
                        for (auto& map: maps_ | std::views::values) {
                            map.rankings = {};
                            map.sector_timestamps = {};
                            map.level_start_timestamp = start_timestamp;
                        }
                    }
                    else {
                        auto& last_map_data = maps_[last_countdown_map_.get_hash_bytes_string()];
                        last_map_data.rankings = {};
                        last_map_data.sector_timestamps = {};
                        last_map_data.level_start_timestamp = start_timestamp;
                    }

                    if ((!force_restart && !map_matches) || !m_bml->IsIngame() || spectator_mode_)
                        return;
                    did_not_finish_ = false;
                    max_sector_ = 1;
                    if (restart_level) {
                        countdown_restart_ = true;
                        restart_current_level();
                    }
                    else if (auto* array_energy = static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(energy_array_))) {
                        int points = (current_map_.level == 1) ? initial_points_ : initial_points_ - 1;
                        array_energy->SetElementValue(0, 0, &points);
                        array_energy->SetElementValue(0, 1, &initial_lives_);
                        counter_start_timestamp_ = start_timestamp;
                    }
                });
                break;
            }
            case ct::Ready:
            case ct::ConfirmReady:
                SendIngameMessage(std::format("[{}]: {}{} - {}",
                                  sender_name, map_name, msg->content.get_level_mode_label(),
                                  msg->content.get_type_label()
                ).c_str());
                utils_.run_on_game_thread([this] { play_countdown_sound(bmmo::countdown_type::Ready); });
                break;
            case ct::Countdown_1:
            case ct::Countdown_2:
            case ct::Countdown_3:
            default:
                SendIngameMessage(std::format("[{}]: {}{} - {}",
                                  sender_name, map_name, msg->content.get_level_mode_label(),
                                  msg->content.get_type_label()).c_str());
                // asio::post(thread_pool_, [this] { play_beep(440, 500); });
                utils_.run_on_game_thread([this, type = msg->content.type] { play_countdown_sound(type); });
                break;
        }
        utils_.flash_window();
        break;
    }
    case bmmo::DidNotFinish: {
        auto* msg = bmmo::message_utils::view_as<bmmo::did_not_finish_msg>(network_msg);
        if (!msg) break;
        auto player_name = get_username(msg->content.player_id);
        SendIngameMessage(std::format(
            "{}{} did not finish {} (furthest reach: sector {}).",
            msg->content.cheated ? "[CHEAT] " : "",
            player_name,
            get_map_name(msg->content.map),
            msg->content.sector
        ), bmmo::color_code(msg->code));
        if (msg->content.player_id == db_.get_client_id())
            did_not_finish_ = true;

        maps_[msg->content.map.get_hash_bytes_string()].rankings.second.push_back({
            (bool)msg->content.cheated, player_name, msg->content.sector });
        play_wave_sound(sound_dnf_);
        utils_.flash_window();
        break;
    }
    case bmmo::HighscoreTimerCalibration: {
        auto* msg = bmmo::message_utils::view_as<bmmo::highscore_timer_calibration_msg>(network_msg);
        if (!msg) break;
        if (current_map_ != msg->content.map) break;
        current_level_mode_ = bmmo::level_mode::Highscore;
        hs_begin_delay_ += msg->content.time_diff_microseconds;
        std::lock_guard lk(bml_mtx_);
        if (map_enter_timestamp_ != 0) {
            force_hs_calibration_ = true;
            break;
        }
        else if (hs_calibrated_) {
            break;
        }
        // the energy array and the time manager are engine state
        utils_.run_on_game_thread([this] {
            auto* energy = static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(energy_array_));
            if (!energy) return;
            int points;
            energy->GetElementValue(0, 0, &points);
            const auto now = m_bml->GetTimeManager()->GetTime();
            points -= (int)std::ceilf((hs_begin_delay_ / 1e3f + counter_start_timestamp_ - now) / point_decrease_interval_);
            energy->SetElementValue(0, 0, &points);
            counter_start_timestamp_ = now - (hs_begin_delay_ / 1e3f);
            move_size_time_length_ = 0;
        });
        break;
    }
    case bmmo::LatencyData: {
        auto msg = bmmo::message_utils::deserialize<bmmo::latency_data_msg>(network_msg);
        for (const auto& [id, ping] : msg.data) {
            //if (! // do nothing for now
            db_.update(id, ping);
            //&& id != db_.get_client_id())
        }
        break;
    }
    case bmmo::LevelFinishV2: {
        auto* msg = bmmo::message_utils::view_as<bmmo::level_finish_v2_msg>(network_msg);
        if (!msg) break;

        // the message points into the receive buffer, so copy it for the
        // deferred call; the callbacks are other mods' code and touch BML
        utils_.run_on_game_thread([this, finish = *msg] {
            auto copy = finish;
            notify_listeners([&copy](const auto& i) { i->on_level_finish(&copy); });
        });

        // Prepare message
        std::string map_name = get_map_name(msg->content.map),
            player_name = get_username(msg->content.player_id),
            formatted_score = msg->content.get_formatted_score();
        //auto state = db_.get(msg->content.player_id);
        //assert(state.has_value() || (db_.get_client_id() == msg->content.player_id));
        SendIngameMessage(std::format(
            "{}{} finished {}{} in {}{} place (score: {}; real time: {}).",
            msg->content.cheated ? "[CHEAT] " : "", player_name,
            map_name, get_level_mode_label(msg->content.mode),
            msg->content.rank, bmmo::string_utils::get_ordinal_suffix(msg->content.rank),
            formatted_score, msg->content.get_formatted_time()),
            bmmo::color_code(msg->code));

        maps_[msg->content.map.get_hash_bytes_string()].rankings.first.push_back({
            (bool)msg->content.cheated, player_name, msg->content.mode,
            msg->content.rank, msg->content.timeElapsed, formatted_score});
        // TODO: Stop displaying objects on finish
        if (msg->content.player_id != db_.get_client_id())
            play_wave_sound(msg->content.cheated ? sound_level_finish_cheat_ : sound_level_finish_);
        utils_.flash_window();
        break;
    }
    case bmmo::LevelFinish:
        break;
    case bmmo::RoomState: {
        bmmo::room_state_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (msg.deserialize())
            handle_room_state(std::move(msg));
        break;
    }
    case bmmo::RoomEvent: {
        bmmo::room_event_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (msg.deserialize())
            handle_room_event(msg);
        break;
    }
    case bmmo::SessionStart: {
        bmmo::session_start_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (msg.deserialize())
            handle_session_start(std::move(msg));
        break;
    }
    case bmmo::SessionAssign: {
        bmmo::session_assign_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (msg.deserialize())
            handle_session_assign(msg);
        break;
    }
    case bmmo::SessionSnapshot: {
        bmmo::session_snapshot_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (msg.deserialize())
            handle_session_snapshot(std::move(msg));
        break;
    }
    case bmmo::SessionRemoteInput: {
        bmmo::session_remote_input_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (msg.deserialize())
            handle_session_remote_input(std::move(msg));
        break;
    }
    case bmmo::SessionEvent: {
        bmmo::session_event_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (msg.deserialize())
            handle_session_event(std::move(msg));
        break;
    }
    case bmmo::SessionEnd: {
        bmmo::session_end_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (msg.deserialize())
            handle_session_end(msg);
        break;
    }
    case bmmo::MapNames: {
        bmmo::map_names_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        merge_map_names(msg.maps);
        break;
    }
    case bmmo::OwnedCheatState: {
        assert(network_msg->m_cbSize == sizeof(bmmo::owned_cheat_state_msg));
        auto* ocs = bmmo::message_utils::view_as<bmmo::owned_cheat_state_msg>(network_msg);
        if (!ocs) break;
        auto state = db_.get(ocs->content.player_id);
        assert(state.has_value() || (db_.get_client_id() == ocs->content.player_id));
        if (state.has_value()) {
            db_.update(ocs->content.player_id, (bool) ocs->content.state.cheated);
        }

        if (ocs->content.state.notify) {
            std::string s = std::format("{} turned cheat [{}].", get_username(ocs->content.player_id), ocs->content.state.cheated ? "on" : "off");
            SendIngameMessage(s.c_str());
        }
        break;
    }
    case bmmo::CheatToggle: {
        auto* msg = bmmo::message_utils::view_as<bmmo::cheat_toggle_msg>(network_msg);
        if (!msg) break;
        bool cheat = msg->content.cheated;
        // EnableCheat drives gameplay scripts; do it on the game thread
        utils_.run_on_game_thread([this, cheat] {
            if (cheat != m_bml->IsCheatEnabled() && !spectator_mode_) {
                notify_cheat_toggle_ = false;
                m_bml->EnableCheat(cheat);
                notify_cheat_toggle_ = true;
                play_wave_sound(sound_knock_);
                utils_.flash_window();
            }
        });
        std::string str = std::format("[Server] toggled{} cheat [{}]{}!",
                                      msg->content.notify ? "" : " your",
                                      cheat ? "on" : "off",
                                      msg->content.notify ? " globally" : "");
        SendIngameMessage(str.c_str(), bmmo::color_code(msg->code));
        break;
    }
    case bmmo::OwnedCheatToggle: {
        auto* msg = bmmo::message_utils::view_as<bmmo::owned_cheat_toggle_msg>(network_msg);
        if (!msg) break;
        std::string player_name = get_username(msg->content.player_id);
        if (player_name != "") {
            bool cheat = msg->content.state.cheated;
            std::string str = std::format("{} toggled cheat [{}] globally!", player_name, cheat ? "on" : "off");
            utils_.run_on_game_thread([this, cheat] {
                if (cheat != m_bml->IsCheatEnabled() && !spectator_mode_) {
                    notify_cheat_toggle_ = false;
                    m_bml->EnableCheat(cheat);
                    notify_cheat_toggle_ = true;
                    play_wave_sound(sound_knock_);
                    utils_.flash_window();
                }
            });
            SendIngameMessage(str.c_str(), bmmo::color_code(msg->code));
        }
        break;
    }
    case bmmo::PlayerKicked: {
        bmmo::player_kicked_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        SendIngameMessage(std::format("{} was kicked by {}{}{}.",
            msg.kicked_player_name,
            (msg.executor_name == "") ? "the server" : msg.executor_name,
            (msg.reason == "") ? "" : " (" + msg.reason + ")",
            msg.crashed ? " and crashed subsequently" : ""
        ).c_str());

        break;
    }
    case bmmo::SimpleAction: {
        auto* msg = bmmo::message_utils::view_as<bmmo::simple_action_msg>(network_msg);
        if (!msg) break;
        switch (msg->content) {
            using sa = bmmo::simple_action;
            case sa::LoginDenied:
                SendIngameMessage("Login denied.");
                break;
            case sa::TriggerFatalError:
                trigger_fatal_error();
                break;
            case sa::CurrentMapQuery: {
                break;
            }
            case sa::Unknown:
            default:
                logger_->Error("Unknown action request received.");
        }
        break;
    }
    case bmmo::OwnedSimpleAction: {
        auto* msg = bmmo::message_utils::view_as<bmmo::owned_simple_action_msg>(network_msg);
        if (!msg) break;
        switch (msg->content.type) {
            using osa = bmmo::owned_simple_action_type;
            case osa::RestartRequestFailed: {
                SendIngameMessage(std::format("{} restart request failed.",
                                  (msg->content.player_id == db_.get_client_id()) ?
                                      "Your" : get_username(msg->content.player_id) + "'s"),
                                  bmmo::ansi::Italic);
                break;
            }
            default:
                break;
        }
        break;
    }
    case bmmo::ActionDenied: {
        auto* msg = bmmo::message_utils::view_as<bmmo::action_denied_msg>(network_msg);
        if (!msg) break;
        SendIngameMessage(("Action failed: " + msg->content.to_string()).c_str(), bmmo::color_code(msg->code));
        break;
    }
    case bmmo::CurrentMap: {
        auto* msg = bmmo::message_utils::view_as<bmmo::current_map_msg>(network_msg);
        if (!msg) break;
        if (msg->content.type == bmmo::current_map_state::Announcement) {
            // .value() would throw out of on_message for a player we never
            // saw connect (e.g. one who left between the two messages)
            const auto player_state = db_.get(msg->content.player_id);
            SendIngameMessage(std::format("{}{} is at the {}{} sector of {}.",
                              // own_cheat_enabled_ rather than IsCheatEnabled():
                              // this runs on the network thread
                              (db_.get_client_id() == msg->content.player_id ? own_cheat_enabled_.load()
                              : (player_state && player_state->cheated)) ? "[CHEAT] " : "",
                              get_username(msg->content.player_id), msg->content.sector,
                              bmmo::string_utils::get_ordinal_suffix(msg->content.sector),
                              get_map_name(msg->content.map)),
                              bmmo::color_code(msg->code));
        }
        else {
            int64_t timestamp = db_.get_timestamp_ms(network_msg->GetTimeReceived());
            db_.update_map(msg->content.player_id, msg->content.map, msg->content.sector, timestamp);
            maps_.try_emplace(msg->content.map.get_hash_bytes_string());
            if (!bmmo::name_validator::is_spectator(get_username(msg->content.player_id)))
                update_sector_timestamp(msg->content.map, msg->content.sector, timestamp);
        }
        break;
    }
    case bmmo::CurrentSector: {
        auto* msg = bmmo::message_utils::view_as<bmmo::current_sector_msg>(network_msg);
        if (!msg) break;
        int64_t timestamp = db_.get_timestamp_ms(network_msg->GetTimeReceived());
        db_.update_sector(msg->content.player_id, msg->content.sector, timestamp);
        auto client_map = db_.get_client_map(msg->content.player_id);
        if (client_map.has_value() && !bmmo::name_validator::is_spectator(get_username(msg->content.player_id)))
            update_sector_timestamp(client_map.value(), msg->content.sector, timestamp);
        break;
    }
    case bmmo::ExtraLife: {
        auto msg = bmmo::message_utils::deserialize<bmmo::extra_life_msg>(network_msg);
        for (auto& data: maps_ | std::views::values)
            data.initial_life_count = 3;
        for (const auto& [hash, goal] : msg.life_count_goals)
            maps_[hash].initial_life_count = goal;
        break;
    }
    case bmmo::NameUpdate: {
        auto msg = bmmo::message_utils::deserialize<bmmo::name_update_msg>(network_msg);
        db_.set_nickname(msg.text_content);
        SendIngameMessage(std::format("Your name has been changed to \"{}\" upon server request.",
                          spectator_mode_ ? bmmo::name_validator::get_spectator_nickname(msg.text_content)
                              : msg.text_content), bmmo::ansi::WhiteInverse);
        break;
    }
    case bmmo::OpState: {
        auto* msg = bmmo::message_utils::view_as<bmmo::op_state_msg>(network_msg);
        if (!msg) break;
        SendIngameMessage(std::format("You have been {} Operator permission.",
                msg->content.op ? "granted" : "removed from"), bmmo::color_code(msg->code));
        break;
    }
    case bmmo::PermanentNotification: {
        auto msg = bmmo::message_utils::deserialize<bmmo::permanent_notification_msg>(network_msg);
        if (msg.text_content.empty()) {
            utils_.schedule_sync_call([=] { permanent_notification_.reset(); });
            SendIngameMessage(std::format("[Bulletin] {} - Content cleared.", msg.title),
                              bmmo::color_code(msg.code));
            break;
        }
        std::string parsed_text = bmmo::string_utils::parse_line_breaks(msg.text_content);
        if (!permanent_notification_) {
            utils_.schedule_sync_call([=] {
                permanent_notification_ = std::make_shared<decltype(permanent_notification_)::element_type>("Bulletin", bmmo::string_utils::utf8_to_ansi(parsed_text).c_str(), 0.2f, 0.036f);
                permanent_notification_->sprite_->SetSize({0.6f, 0.12f});
                permanent_notification_->sprite_->SetPosition({0.2f, 0.036f});
                permanent_notification_->sprite_->SetAlignment(CKSPRITETEXT_CENTER);
                permanent_notification_->sprite_->SetFont(utils::get_system_font(), utils_.get_display_font_size(11.72f), 400, false, false);
                permanent_notification_->sprite_->SetZOrder(65536);
                permanent_notification_->paint(player_list_color_);
                permanent_notification_->set_visible(true);
            });
        }
        else
            utils_.schedule_sync_call([=] { permanent_notification_->update(bmmo::string_utils::utf8_to_ansi(parsed_text).c_str()); });
        SendIngameMessage(std::format("[Bulletin] {}: {}", msg.title, 
#ifdef BMMO_USE_BML_PLUS
                          (loader_version_ >= BMLVersion{ 0, 3, 9 }) ?
                          bmmo::string_utils::parse_line_breaks(msg.text_content, " ↳ ") :
#endif // BMMO_USE_BML_PLUS
                          msg.text_content), bmmo::color_code(msg.code));
        utils_.flash_window();
        play_wave_sound(sound_notification_, !utils_.is_foreground_window());
        break;
    }
    case bmmo::PlainText: {
        bmmo::plain_text_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        SendIngameMessage(msg.text_content.c_str());
        utils_.flash_window();
        break;
    }
    case bmmo::PublicNotification: {
        auto msg = bmmo::message_utils::deserialize<bmmo::public_notification_msg>(network_msg);
        SendIngameMessage("[BMMO/" + msg.get_type_name() + "] " + msg.text_content, msg.get_ansi_color_code());
        utils_.flash_window();
        play_wave_sound(sound_knock_);
        break;
    }
    case bmmo::RealWorldTimestamp: {
        auto msg = bmmo::message_utils::deserialize<bmmo::real_world_timestamp_msg>(network_msg);
        server_realworld_timestamp_us_ = msg.content + int64_t(get_status().m_nPing) * 1000LL;
        server_realworld_timestamp_timestamp_ = SteamNetworkingUtils()->GetLocalTimestamp();
        if (!spectator_label_) break;
        using namespace std::chrono;
        const auto real_time = system_clock::to_time_t(system_clock::time_point{microseconds{server_realworld_timestamp_us_}});
        char output[32]; std::tm buf; localtime_s(&buf, &real_time);
        std::strftime(output, sizeof(output), "%F %T", &buf);
        // the label is a sprite: read and update it on the game thread, where
        // cleanup() cannot free it from under us mid-update
        utils_.run_on_game_thread([this, time_str = std::string(output)] {
            if (!spectator_label_) return;
            std::string text = spectator_label_->sprite_->GetText();
            // "[Spectator Mode]" -> "[Real time: %s | Spectator Mode]"
            const auto left_bracket_pos = text.find('['), pipe_pos = text.find(" | ");
            if (left_bracket_pos == text.npos) return;
            if (pipe_pos == text.npos)
                text.insert(left_bracket_pos + 1, "Real time: " + time_str + " | ");
            else
                text.replace(left_bracket_pos + 1, pipe_pos - (left_bracket_pos + 1), "Real time: " + time_str);
            spectator_label_->update(bmmo::string_utils::utf8_to_ansi(text));
        });
        break;
    }
    case bmmo::RestartRequest: {
        auto msg = bmmo::message_utils::deserialize<bmmo::restart_request_msg>(network_msg);
        const bool restart = (msg.content.victim == db_.get_client_id());
        SendIngameMessage(std::format("{} requested to restart {} current level.",
                          get_username(msg.content.requester),
                          restart ? "your" : get_username(msg.content.victim) + "'s"),
                          bmmo::color_code(msg.code));
        if (restart) {
            // restart_current_level() activates a behavior IO in the engine's
            // script graph, and IsIngame() is engine state - both belong on the
            // game thread, like the identical call in the Countdown handler
            utils_.run_on_game_thread([this, requester = msg.content.requester] {
                if (m_bml->IsIngame() && !spectator_mode_)
                    restart_current_level();
                else {
                    send(bmmo::owned_simple_action_msg{.content = {
                        .type = bmmo::owned_simple_action_type::RestartRequestFailed,
                        .player_id = requester,
                    }});
                }
            });
        }
        break;
    }
    case bmmo::ScoreList: {
        auto msg = bmmo::message_utils::deserialize<bmmo::score_list_msg>(network_msg);
        asio::post(thread_pool_, [this, msg = std::move(msg)]() mutable {
            bool hs_mode = (msg.mode == bmmo::level_mode::Highscore);
            bmmo::ranking_entry::sort_rankings(msg.rankings, hs_mode);
            auto formatted_texts = bmmo::ranking_entry::get_formatted_rankings(
                    msg.rankings, get_map_name(msg.map), hs_mode);
            size_t size = msg.rankings.first.size() + msg.rankings.second.size() + 1;
            std::string text; text.reserve(size * 64);
            for (const auto& [line, color] : formatted_texts) {
                text += line + '\n';

                console_window_.print_text(line.c_str(), color);
                logger_->Info("%s", line.c_str());
            }
            utils_.display_important_notification(text, 16.7f - 0.25f * std::clamp(size, 7u, 36u), size + 1, 400);
        });
        break;
    }
    case bmmo::SoundData: {
        auto msg = bmmo::message_utils::deserialize<bmmo::sound_data_msg>(network_msg);
        if (!msg.caption.empty())
            SendIngameMessage("Now playing: " + msg.caption);
        asio::post(thread_pool_, [this, msg = std::move(msg)] {
            utils_.flash_window();
            logger_->Info("Playing sound from server%s", msg.caption.empty() ? "" : (": " + msg.caption).c_str());
            std::stringstream data_text;
            for (const auto& [frequency, duration]: msg.sounds) {
                data_text << ", (" << frequency << ", " << duration << ")";
                play_beep(frequency, duration);
            }
            logger_->Info("Finished playing sound: [%s]", data_text.str().erase(0, 2).c_str());
        });
        break;
    }
    case bmmo::SoundStream: {
        bmmo::sound_stream_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (!msg.deserialize()) {
            SendIngameMessage("Error receiving sound!");
            break;
        }
        if (!msg.caption.empty())
            SendIngameMessage("Now playing: " + msg.caption);
        utils_.flash_window();
        // Creating and playing a CKWaveSound is engine work, and
        // received_wave_sounds_ is shared with the game thread, so all of it
        // runs there. The lifetime is handled by a BML timer rather than a
        // detached sleeping thread: the old code spawned one unbounded thread
        // per message and created CK objects on it.
        // copy the fields out: the message holds a stringstream, so a lambda
        // capturing it would not be copy-constructible (std::function needs it)
        utils_.run_on_game_thread([this, path = msg.path, caption = msg.caption,
                                   gain = msg.gain, pitch = msg.pitch,
                                   duration_ms = msg.duration_ms] {
            // rfind, not find_last_of: the latter matches any one of those
            // characters anywhere, and returns npos for a path containing none
            // of them, which then throws out of substr
            auto name_pos = path.rfind("BMMO_");
            std::string sound_name = "MMO_Sound" + (name_pos == std::string::npos ? path : path.substr(name_pos));
            /* WIP: if (msg.type == bmmo::sound_stream_msg::sound_type::Wave) {} */
            CKWaveSound* sound = nullptr;
            std::string sound_path = path;
            load_wave_sound(&sound, sound_name.data(), sound_path.data(), gain, pitch, false);
            if (!sound) {
                logger_->Warn("Failed to load sound <%s> from server.", sound_name.c_str());
                return;
            }
            {
                std::lock_guard lk(bml_mtx_);
                received_wave_sounds_.insert(sound);
            }
            logger_->Info("Playing sound <%s> from server%s",
                              sound_name.c_str(),
                              caption.empty() ? "" : (": " + caption).c_str());
            int duration = int(duration_ms);
            if (duration <= 0 || duration >= sound->GetSoundLength())
                duration = sound->GetSoundLength() + 1000;
            logger_->Info("Sound length: %d / %.2f = %.0f milliseconds; Gain: %.2f; Pitch: %.2f",
                              duration, pitch, duration / pitch, gain, pitch);
            sound->Play();

            m_bml->AddTimer(float(duration / pitch), [this, sound] {
                std::lock_guard lk(bml_mtx_);
                if (!received_wave_sounds_.contains(sound))
                    return;
                destroy_wave_sound(sound, true);
                received_wave_sounds_.erase(sound);
            });
        });
        break;
    }
    case bmmo::PopupBox: {
        auto msg = bmmo::message_utils::deserialize<bmmo::popup_box_msg>(network_msg);
        SendIngameMessage("[Popup] {" + msg.title + "}: " + msg.text_content, bmmo::color_code(msg.code));
        std::thread([this, msg = std::move(msg)] {
            std::ignore = MessageBoxW(utils_.get_main_window(),
                                      bmmo::string_utils::ConvertUtf8ToWide(msg.text_content).c_str(),
                                      bmmo::string_utils::ConvertUtf8ToWide(msg.title).c_str(),
                                      MB_OK | MB_ICONINFORMATION);
        }).detach();
        break;
    }
    default:
        logger_->Error("Invalid message with opcode %d received.", raw_msg->code);
        break;
    }
}

void BallanceMMOClient::LoggingOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg)
{
    static constexpr const char* fmt_string = "[%d] %10.6f %s";
    SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
    switch (eType) {
        case k_ESteamNetworkingSocketsDebugOutputType_Bug:
        case k_ESteamNetworkingSocketsDebugOutputType_Error:
            get_instance()->logger_->Error(fmt_string, eType, time * 1e-6, pszMsg);
            break;
        case k_ESteamNetworkingSocketsDebugOutputType_Important:
        case k_ESteamNetworkingSocketsDebugOutputType_Warning:
            get_instance()->logger_->Warn(fmt_string, eType, time * 1e-6, pszMsg);
            break;
        default:
            get_instance()->logger_->Info(fmt_string, eType, time * 1e-6, pszMsg);
    }

    if (eType == k_ESteamNetworkingSocketsDebugOutputType_Bug) {
        get_instance()->SendIngameMessage("BallanceMMO has encountered a bug which is fatal. Please contact developer with this piece of log.");
        terminate(5);
    }
}
