#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
    BallanceMMOClient::init_socket();
    return new BallanceMMOClient(bml);
}

void BindCrtHandlesToStdHandles(bool bindStdIn, bool bindStdOut, bool bindStdErr) {
    // Re-initialize the C runtime "FILE" handles with clean handles bound to "nul". We do this because it has been
    // observed that the file number of our standard handle file objects can be assigned internally to a value of -2
    // when not bound to a valid target, which represents some kind of unknown internal invalid state. In this state our
    // call to "_dup2" fails, as it specifically tests to ensure that the target file number isn't equal to this value
    // before allowing the operation to continue. We can resolve this issue by first "re-opening" the target files to
    // use the "nul" device, which will place them into a valid state, after which we can redirect them to our target
    // using the "_dup2" function.
    if (bindStdIn) {
        FILE* dummyFile;
        freopen_s(&dummyFile, "nul", "r", stdin);
    }
    if (bindStdOut) {
        FILE* dummyFile;
        freopen_s(&dummyFile, "nul", "w", stdout);
    }
    if (bindStdErr) {
        FILE* dummyFile;
        freopen_s(&dummyFile, "nul", "w", stderr);
    }

    // Redirect unbuffered stdin from the current standard input handle
    if (bindStdIn) {
        HANDLE stdHandle = GetStdHandle(STD_INPUT_HANDLE);
        if (stdHandle != INVALID_HANDLE_VALUE) {
            int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
            if (fileDescriptor != -1) {
                FILE* file = _fdopen(fileDescriptor, "r");
                if (file != NULL) {
                    int dup2Result = _dup2(_fileno(file), _fileno(stdin));
                    if (dup2Result == 0) {
                        setvbuf(stdin, NULL, _IONBF, 0);
                    }
                }
            }
        }
    }

    // Redirect unbuffered stdout to the current standard output handle
    if (bindStdOut) {
        HANDLE stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (stdHandle != INVALID_HANDLE_VALUE) {
            int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
            if (fileDescriptor != -1) {
                FILE* file = _fdopen(fileDescriptor, "w");
                if (file != NULL) {
                    int dup2Result = _dup2(_fileno(file), _fileno(stdout));
                    if (dup2Result == 0) {
                        setvbuf(stdout, NULL, _IONBF, 0);
                    }
                }
            }
        }
    }

    // Redirect unbuffered stderr to the current standard error handle
    if (bindStdErr) {
        HANDLE stdHandle = GetStdHandle(STD_ERROR_HANDLE);
        if (stdHandle != INVALID_HANDLE_VALUE) {
            int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
            if (fileDescriptor != -1) {
                FILE* file = _fdopen(fileDescriptor, "w");
                if (file != NULL) {
                    int dup2Result = _dup2(_fileno(file), _fileno(stderr));
                    if (dup2Result == 0) {
                        setvbuf(stderr, NULL, _IONBF, 0);
                    }
                }
            }
        }
    }

    // Clear the error state for each of the C++ standard stream objects. We need to do this, as attempts to access the
    // standard streams before they refer to a valid target will cause the iostream objects to enter an error state. In
    // versions of Visual Studio after 2005, this seems to always occur during startup regardless of whether anything
    // has been read from or written to the targets or not.
    if (bindStdIn) {
        std::wcin.clear();
        std::cin.clear();
    }
    if (bindStdOut) {
        std::wcout.clear();
        std::cout.clear();
    }
    if (bindStdErr) {
        std::wcerr.clear();
        std::cerr.clear();
    }
  }

bool BallanceMMOClient::show_console() {
    if (AllocConsole()) {
        SetConsoleTitle("BallanceMMO Console");
        if (HWND hwnd = GetConsoleWindow()) {
            if (HMENU hMenu = GetSystemMenu(hwnd, FALSE)) {
                EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
            }
        }
        /*old_stdin = _dup(_fileno(stdin));
        old_stdout = _dup(_fileno(stdout));
        old_stderr = _dup(_fileno(stderr));*/
        BindCrtHandlesToStdHandles(true, true, true);
        /*        freopen("CONIN$", "r", stdin);
                freopen("CONOUT$", "w", stdout);
                freopen("CONOUT$", "w", stderr);*/
        if (console_thread_.joinable())
            console_thread_.join();
        console_thread_ = std::thread([&]() {
            console_running_ = true;
            DWORD mode;
            GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
            SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            _setmode(_fileno(stdin), _O_U16TEXT);
            for (const auto& i : previous_msg_) {
                Printf(i.c_str());
            }
            while (true) {
                std::cout << "\r> " << std::flush;
                std::wstring wline;
                /*wchar_t wc;
                do {
                    wc = _getwch();
                    wline += wc;
                    ungetwc(wc, stdin);
                    std::wcout << wc;
                }
                while (wc != L'\n');
                std::cout << '\n';*/
                std::getline(std::wcin, wline);
                // std::wcout << wline << std::endl;
                if (!console_running_)
                    break;
                std::string line = bmmo::message_utils::ConvertWideToANSI(wline),
                  cmd = "ballancemmo";
                if (auto pos = line.rfind('\r'); pos != std::string::npos)
                    line.erase(pos);
                std::vector<std::string> args;
                bmmo::command_parser parser(line);
                while (!cmd.empty()) {
                    args.push_back(cmd);
                    cmd = parser.get_next_word();
                }
                if (args[1] == "mmo" || args[1] == "ballancemmo")
                    args.erase(args.begin());
                GetLogger()->Info("Execute command from console: %s", line.c_str());
                OnCommand(m_bml, args);
            }
        });
        return true;
    };
    return false;
}

bool BallanceMMOClient::hide_console() {
    console_running_ = false;/*
    _dup2(old_stdin, _fileno(stdin));
    _dup2(old_stdout, _fileno(stdout));
    _dup2(old_stderr, _fileno(stderr));*/
    /*fclose(stdin);
    fclose(stdout);
    fclose(stderr);*/
    //std::this_thread::sleep_for(std::chrono::milliseconds(100));
    //TerminateThread(console_thread_.native_handle(), 0);
    if (FreeConsole()) {
        return true;
    }
    return false;
}

void BallanceMMOClient::show_player_list() {
    // player_list_thread has a sleep_for and may freeze the game if joined synchronously
    asio::post(thread_pool_, [this] {
        player_list_visible_ = false;
        if (player_list_thread_.joinable())
            player_list_thread_.join();
        player_list_thread_ = std::thread([&] {
            text_sprite player_list("PlayerList", "", RIGHT_MOST, 0.412f);
            player_list.sprite_->SetSize({RIGHT_MOST, 0.588f});
            player_list.sprite_->SetZOrder(128);
            player_list.sprite_->SetFont(system_font_, get_display_font_size(9.65f), 400, false, false);
            player_list.paint(player_list_color_);
            player_list.set_visible(true);
            player_list_visible_ = true;
            struct list_entry { std::string map_name, name; int sector; int32_t timestamp; bool cheated; };
            while (player_list_visible_) {
                std::vector<list_entry> status_list;
                status_list.reserve(db_.player_count() + !spectator_mode_);
                db_.for_each([&](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
                    if (pair.first == db_.get_client_id() || bmmo::name_validator::is_spectator(pair.second.name))
                        return true;
                    status_list.push_back({ pair.second.current_map_name, pair.second.name, pair.second.current_sector, pair.second.current_sector_timestamp, pair.second.cheated });
                    return true;
                });
                if (!spectator_mode_)
                    status_list.push_back({ current_map_.get_display_name(), db_.get_nickname(), current_sector_, current_sector_timestamp_, m_bml->IsCheatEnabled() });
                std::sort(status_list.begin(), status_list.end(), [](const auto& i1, const auto& i2) {
                    const int map_cmp = boost::to_lower_copy(i1.map_name).compare(boost::to_lower_copy(i2.map_name));
                    if (map_cmp > 0) return true;
                    if (map_cmp == 0) {
                      const int sector_cmp = i1.sector - i2.sector;
                      if (sector_cmp != 0) return sector_cmp > 0;
                      if (i1.sector != 1) {
                        const int32_t time_cmp = i1.timestamp - i2.timestamp;
                        if (time_cmp != 0) return time_cmp < 0;
                      }
                      return boost::ilexicographical_compare(i1.name, i2.name);
                    }
                    return false;
                }); // two comparisons don't work for some unknown reason
                auto size = status_list.size();
                std::string text = std::to_string(size) + " player" + ((size == 1) ? "" : "s") + " online:\n";
                text.reserve(1024);
                for (const auto& i: status_list /* | std::views::reverse */)
                    text.append(std::format("{}{}: {}, S{:02d}\n", i.name, i.cheated ? " [C]" : "", i.map_name, i.sector));
                player_list.update(text);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
    });
}

void BallanceMMOClient::OnLoad()
{
    init_config();
    //client_ = std::make_unique<client>(GetLogger(), m_bml);
    input_manager_ = m_bml->GetInputManager();
}

void BallanceMMOClient::OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj)
{
    if (strcmp(filename, "3D Entities\\Balls.nmo") == 0) {
        objects_.destroy_all_objects();
        objects_.init_template_balls();
        //objects_.init_players();
        md5_from_file("..\\3D Entities\\Balls.nmo", balls_nmo_md5_);
    }

    if (strcmp(filename, "3D Entities\\Gameplay.nmo") == 0) {
        current_level_array_ = CKOBJID(m_bml->GetArrayByName("CurrentLevel"));
        ingame_parameter_array_ = CKOBJID(m_bml->GetArrayByName("IngameParameter"));
    }

    if (isMap) {
        GetLogger()->Info("Initializing peer objects...");
        objects_.init_players();
        boost::regex name_pattern(".*\\\\(Level|Maps)\\\\(.*).nmo", boost::regex::icase);
        std::string path(filename);
        boost::smatch matched;
        if (boost::regex_search(path, matched, name_pattern)) {
            current_map_.name = matched[2].str();
            if (boost::iequals(matched[1].str(), "Maps")) {
                current_map_.type = bmmo::map_type::CustomMap;
            }
            else {
                current_map_.type = bmmo::map_type::OriginalLevel;
                path = "..\\" + path;
            }
        } else {
            current_map_.name = std::string(filename);
            current_map_.type = bmmo::map_type::Unknown;
        }
        md5_from_file(path, current_map_.md5);
        static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(current_level_array_))->GetElementValue(0, 0, &current_map_.level);
        if (connected()) {
            if (!map_names_.contains(current_map_.get_hash_bytes_string())) {
                map_names_[current_map_.get_hash_bytes_string()] = current_map_.name;
                send_current_map_name();
            }
            player_ball_ = get_current_ball();
            if (player_ball_ != nullptr) {
                local_state_handler_->poll_and_send_state_forced(player_ball_);
            }
            OnPostCheckpointReached();
            send_current_map();
        };
        GetLogger()->Info("Current map: %s; type: %d; md5: %s.",
            current_map_.name.c_str(), (int)current_map_.type, current_map_.get_hash_string().c_str());
        reset_timer_ = true;
        GetLogger()->Info("Initialization completed.");
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
}

void BallanceMMOClient::OnPostCheckpointReached() {
    if (get_current_sector() && connected()) send_current_sector();
}

void BallanceMMOClient::OnPostExitLevel() {
    OnPostCheckpointReached();
}

void BallanceMMOClient::OnCounterActive() {
    OnPostCheckpointReached();
}

void BallanceMMOClient::OnPostStartMenu()
{
    if (init_) {
        GetLogger()->Info("Destroying peer objects...");
        objects_.destroy_all_objects();
        GetLogger()->Info("Destroy completed.");
    }
    else {
        ping_ = std::make_shared<text_sprite>("T_MMO_PING", "", RIGHT_MOST, 0.03f);
        ping_->sprite_->SetSize(Vx2DVector(RIGHT_MOST, 0.4f));
        ping_->sprite_->SetFont("Arial", get_display_font_size(10), 500, false, false);
        status_ = std::make_shared<text_sprite>("T_MMO_STATUS", "Disconnected", RIGHT_MOST, 0.0f);
        status_->sprite_->SetFont("Times New Roman", get_display_font_size(11), 700, false, false);
        status_->paint(0xffff0000);

        using namespace std::placeholders;
        
        m_bml->RegisterCommand(new CommandMMO(std::bind(&BallanceMMOClient::OnCommand, this, _1, _2), std::bind(&BallanceMMOClient::OnTabComplete, this, _1, _2)));
        m_bml->RegisterCommand(new CommandMMOSay([this](IBML* bml, const std::vector<std::string>& args) { OnCommand(bml, args); }));

        edit_Gameplay_Tutorial(m_bml->GetScriptByName("Gameplay_Tutorial"));

        init_ = true;
    }

    validate_nickname(props_["playername"]);
}

void BallanceMMOClient::OnProcess() {
    //poll_connection_state_changes();
    //poll_incoming_messages();

    //poll_status_toggle();
    poll_local_input();

    if (!connected())
        return;

    std::unique_lock<std::mutex> bml_lk(bml_mtx_, std::try_to_lock);

    if (m_bml->IsIngame() && bml_lk) {
        const auto current_timestamp = SteamNetworkingUtils()->GetLocalTimestamp();

        objects_.update(current_timestamp, db_.flush());

        if (current_timestamp >= next_update_timestamp_) {
            if (current_timestamp - next_update_timestamp_ > 1048576)
                next_update_timestamp_ = current_timestamp;
            next_update_timestamp_ += MINIMUM_UPDATE_INTERVAL;

            auto ball = get_current_ball();
            if (player_ball_ == nullptr)
                player_ball_ = ball;

            check_on_trafo(ball);
            local_state_handler_->poll_and_send_state(ball);
        }
    }
}

void BallanceMMOClient::OnStartLevel()
{
    /*if (!connected())
        return;*/

    player_ball_ = get_current_ball();
    if (local_state_handler_ != nullptr)
        local_state_handler_->set_ball_type(db_.get_ball_id(player_ball_->GetName()));

    if (reset_timer_) {
        level_start_timestamp_[current_map_.get_hash_bytes_string()] = m_bml->GetTimeManager()->GetTime();
        reset_timer_ = false;
    }

    m_bml->AddTimer(10u, [this]() {
        if (current_map_.level == 1 && countdown_restart_ && connected()) {
            auto* tutorial_exit = static_cast<CKBehaviorIO*>(m_bml->GetCKContext()->GetObject(tutorial_exit_event_));
            tutorial_exit->Activate();
            countdown_restart_ = false;
        }

        OnCounterActive();
    });

    //objects_.destroy_all_objects();
    //objects_.init_players();
}

// may give wrong values of extra points
void BallanceMMOClient::OnLevelFinish() {
    if (!connected() || spectator_mode_)
        return;

    // Sending level finish messages immediately may get us wrong values of
    // extra points. We have to wait for some time.
    // IBML::AddTimer is based on frames; this may be unfair to players with
    // low framerates, so we use our thread pool with this_thread::sleep_for.
    asio::post(thread_pool_, [this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        bmmo::level_finish_v2_msg msg{};
        auto* array_energy = m_bml->GetArrayByName("Energy");
        array_energy->GetElementValue(0, 0, &msg.content.points);
        array_energy->GetElementValue(0, 1, &msg.content.lives);
        array_energy->GetElementValue(0, 5, &msg.content.lifeBonus);
        static_cast<CKDataArray*>(m_bml->GetCKContext()->GetObject(current_level_array_))->GetElementValue(0, 0, &current_map_.level);
        m_bml->GetArrayByName("AllLevel")->GetElementValue(current_map_.level - 1, 6, &msg.content.levelBonus);
        msg.content.timeElapsed = (m_bml->GetTimeManager()->GetTime() - level_start_timestamp_[current_map_.get_hash_bytes_string()]) / 1e3f;
        reset_timer_ = true;
        msg.content.cheated = m_bml->IsCheatEnabled();
        msg.content.map = current_map_;
        GetLogger()->Info("Sending level finish message...");

        send(msg, k_nSteamNetworkingSend_Reliable);
    });
}

void BallanceMMOClient::OnLoadScript(CKSTRING filename, CKBehavior* script)
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
    if (!connected() || spectator_mode_)
        return;
    bmmo::cheat_state_msg msg{};
    msg.content.cheated = enable;
    msg.content.notify = notify_cheat_toggle_;
    send(msg, k_nSteamNetworkingSend_Reliable);
}

void BallanceMMOClient::OnModifyConfig(CKSTRING category, CKSTRING key, IProperty* prop) {
    if (prop == props_["playername"]) {
        validate_nickname(prop);
    }
    else if (prop == props_["uuid"]) {
        prop->SetString(boost::uuids::to_string(uuid_).c_str());
        GetLogger()->Warn("Warning: Unable to modify UUID.");
    }
    else if (prop == props_["extrapolation"]) {
        objects_.toggle_extrapolation(prop->GetBoolean());
    }
    else if (prop == props_["spectator"]) {
        if (connected() || connecting()) {
            disconnect_from_server();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            connect_to_server(server_addr);
        }
    }
    else if (prop == props_["player_list_color"]) {
        parse_and_set_player_list_color(prop);
    }
}

void BallanceMMOClient::OnExitGame()
{
    cleanup(true);
    client::destroy();
}

//void BallanceMMOClient::OnUnload() {
//    cleanup(true);
//    client::destroy();
//}

void BallanceMMOClient::OnCommand(IBML* bml, const std::vector<std::string>& args)
{
    auto help = [this](IBML* bml) {
        std::lock_guard<std::mutex> lk(bml_mtx_);
        SendIngameMessage("BallanceMMO Help");
        SendIngameMessage(std::format("Version: {}; build time: {} {}.",
                                      version_string, __DATE__, __TIME__));
        SendIngameMessage("/mmo connect - Connect to server.");
        SendIngameMessage("/mmo disconnect - Disconnect from server.");
        SendIngameMessage("/mmo list - List online players.");
        SendIngameMessage("/mmo say - Send message to each other.");
    };

    const size_t length = args.size();
    std::string lower1;
    if (length > 1) lower1 = boost::algorithm::to_lower_copy(args[1]);

    switch (length) {
        case 1: {
            help(bml);
            return;
        }
        case 2: {
            if (lower1 == "connect" || lower1 == "c") {
                connect_to_server();
            }
            else if (lower1 == "disconnect" || lower1 == "d") {
                disconnect_from_server();
            }
            else if (lower1 == "list" || lower1 == "l" || lower1 == "list-id" || lower1 == "li") {
                if (!connected())
                    return;

                bool show_id = (lower1 == "list-id" || lower1 == "li");
                
                typedef std::tuple<HSteamNetConnection, std::string, bool> player_data;
                std::vector<player_data> players, spectators;
                players.reserve(db_.player_count() + 1);
                db_.for_each([&](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
                    if (pair.first == db_.get_client_id())
                        return true;
                    if (bmmo::name_validator::is_spectator(pair.second.name))
                        spectators.emplace_back(pair.first, pair.second.name, pair.second.cheated);
                    else
                        players.emplace_back(pair.first, pair.second.name, pair.second.cheated);
                    return true;
                });
                if (spectator_mode_)
                    spectators.emplace_back(db_.get_client_id(), db_.get_nickname(), m_bml->IsCheatEnabled());
                else
                    players.emplace_back(db_.get_client_id(), db_.get_nickname(), m_bml->IsCheatEnabled());
                int player_count = players.size();
                std::ranges::sort(players, [](const player_data& i1, const player_data& i2)
                    { return boost::algorithm::ilexicographical_compare(std::get<1>(i1), std::get<1>(i2)); });
                players.insert(players.begin(), spectators.begin(), spectators.end());

                SendIngameMessage(std::format("{} player{} and {} spectator{} ({} total) online:",
                                              player_count, player_count == 1 ? "" : "s",
                                              spectators.size(), spectators.size() == 1 ? "" : "s",
                                              players.size()));
                std::string line; player_count = players.size();
                for (int i = 0; i < player_count; ++i) {
                    const auto& [id, name, cheated] = players[i];
                    line.append(name + (cheated ? " [CHEAT]" : "")
                                + (show_id ? (": " + std::to_string(id)) : ""));
                    if (i != player_count - 1) line.append(", ");
                    if (line.length() > 80) {
                        SendIngameMessage(line);
                        line.clear();
                    }
                }

                if (!line.empty())
                    SendIngameMessage(line);
            }
            else if (lower1 == "dnf") {
                bmmo::did_not_finish_msg msg{};
                if (current_map_.level == 0 || spectator_mode_)
                    return;
                msg.content.sector = current_sector_;
                msg.content.map = current_map_;
                msg.content.cheated = m_bml->IsCheatEnabled();
                send(msg, k_nSteamNetworkingSend_Reliable);
            }
            else if (lower1 == "show") {
                if (console_running_) {
                    SendIngameMessage("Error: console is already visible.");
                    return;
                }
                /*_dup2(old_stdin, _fileno(stdin));
                _dup2(old_stdout, _fileno(stdout));
                _dup2(old_stderr, _fileno(stderr));*/
                show_console();
            }
            else if (lower1 == "hide") {
                if (!console_running_) {
                    SendIngameMessage("Error: console is already hidden.");
                    return;
                }
                hide_console();
            }
            else if (lower1 == "getpos" || lower1 == "gp") {
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
                    std::string type = std::unordered_map<int, std::string>{ {0, "paper"}, {1, "stone"}, {2, "wood"} }[i.second.type];
                    if (type.empty()) type = "unknown (id #" + std::to_string(i.second.type) + ")";
                    SendIngameMessage(std::format("{} is at {:.2f}, {:.2f}, {:.2f} with {} ball.",
                                      i.first,
                                      i.second.position.x, i.second.position.y, i.second.position.z,
                                      type
                    ));
                }
            }
            else if (lower1 == "getmap" || lower1 == "gm") {
                if (!connected())
                    return;
                db_.for_each([&](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
                    if (pair.first == db_.get_client_id())
                        return true;
                    SendIngameMessage(std::format("{}{} is at the {}{} sector of {}.",
                                      pair.second.cheated ? "[CHEAT] " : "",
                                      pair.second.name, pair.second.current_sector,
                                      bmmo::get_ordinal_rank(pair.second.current_sector),
                                      pair.second.current_map_name));
                    return true;
                });
            }
            else if (lower1 == "announcemap" || lower1 == "am") {
                if (!connected())
                    return;
                send_current_map(bmmo::current_map_state::Announcement);
            }
            else if (lower1 == "reload" || lower1 == "rl") {
                if (!connected() || !m_bml->IsIngame())
                    return;
                objects_.reload();
                SendIngameMessage("Reload completed.");
            }
            else if (lower1 == "gettimestamp") {
                db_.for_each([this](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
                    SendIngameMessage(std::format("{}: last recalibrated timestamp: {}; mean time diff: {:.4f} s.",
                                      pair.second.name,
                                      pair.second.ball_state.front().timestamp, pair.second.time_diff / 1e6l));
                    return true;
                });
            }
            else if (lower1 == "countdown") {
                if (!connected() || !m_bml->IsIngame())
                    return;
                asio::post(thread_pool_, [this]() {
                    for (int i = 3; i >= 0; --i) {
                        send_countdown_message(static_cast<bmmo::countdown_type>(i));
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                });
            }
            else if (lower1 == "ready") {
                if (!connected() || !m_bml->IsIngame() || spectator_mode_)
                    return;
                bmmo::player_ready_msg msg{.content = {.ready = true}};
                send(msg, k_nSteamNetworkingSend_Reliable);
            }
            else if (lower1 == "ready-cancel") {
                if (!connected() || !m_bml->IsIngame() || spectator_mode_)
                    return;
                bmmo::player_ready_msg msg{.content = {.ready = false}};
                send(msg, k_nSteamNetworkingSend_Reliable);
            }
            /*else if (lower1 == "p") {
                objects_.physicalize_all();
            } else if (lower1 == "f") {
                ExecuteBB::SetPhysicsForce(player_ball_, VxVector(0, 0, 0), player_ball_, VxVector(1, 0, 0), m_bml->Get3dObjectByName("Cam_OrientRef"), .43f);
            } else if (lower1 == "u") {
                ExecuteBB::UnsetPhysicsForce(player_ball_);
            }*/
            return;
        }
        case 3: {
            if (lower1 == "connect" || lower1 == "c") {
                connect_to_server(args[2]);
                return;
            }
            else if (lower1 == "cheat") {
                bool cheat_state = false;
                if (boost::iequals(args[2], "on"))
                    cheat_state = true;
                bmmo::cheat_toggle_msg msg{};
                msg.content.cheated = cheat_state;
                send(msg, k_nSteamNetworkingSend_Reliable);
                return;
            }
            else if (lower1 == "rank" && boost::iequals(args[2], "reset")) {
                reset_rank_ = true;
                return;
            }
            else if (lower1 == "teleport" || lower1 == "tp") {
                if (!(connected() && m_bml->IsIngame() && m_bml->IsCheatEnabled()))
                    return;
                std::optional<PlayerState> state;
                if (args[2][0] == '#')
                    state = db_.get((HSteamNetConnection) atoll(args[2].substr(1).c_str()));
                else
                    state = db_.get_from_nickname(args[2]);
                if (!state.has_value()) {
                    SendIngameMessage("Error: requested player \"" + args[2] + "\" does not exist.");
                    return;
                }
                const std::string name = state.value().name;
                const VxVector& position = state.value().ball_state.front().position;

                CKMessageManager* mm = m_bml->GetMessageManager();
                CKMessageType ballDeact = mm->AddMessageType("BallNav deactivate");
                mm->SendMessageSingle(ballDeact, m_bml->GetGroupByName("All_Gameplay"));
                mm->SendMessageSingle(ballDeact, m_bml->GetGroupByName("All_Sound"));

                m_bml->AddTimer(2u, [this, &position, name]() {
                    ExecuteBB::Unphysicalize(get_current_ball());
                    get_current_ball()->SetPosition(position);
                    CK3dEntity* camMF = m_bml->Get3dEntityByName("Cam_MF");
                    VxMatrix matrix = camMF->GetWorldMatrix();
                    m_bml->RestoreIC(camMF, true);
                    camMF->SetPosition(position);
                    camMF->SetWorldMatrix(matrix);
                    m_dynamicPos->ActivateInput(0);
                    m_dynamicPos->Activate();
                    m_phyNewBall->ActivateInput(0);
                    m_phyNewBall->Activate();
                    m_phyNewBall->GetParent()->Activate();
                    SendIngameMessage(std::format("Teleported to \"{}\" at ({:.3f}, {:.3f}, {:.3f}).",
                                      name, position.x, position.y, position.z));
                });
                return;
            }
            else if (lower1 == "custommap") {
                if (args[2] == "reset") { send_current_map(); return; }
                bmmo::map map; srand((uint32_t)time(nullptr));
                for (auto& i: map.md5) i = rand() % std::numeric_limits<uint8_t>::max();
                bmmo::map_names_msg name_msg{};
                name_msg.maps.emplace(map.get_hash_bytes_string(), args[2]);
                name_msg.serialize();
                send(name_msg.raw.str().data(), name_msg.size(), k_nSteamNetworkingSend_Reliable);
                send(bmmo::current_map_msg{.content = {.map = map, .type = bmmo::current_map_state::EnteringMap}}, k_nSteamNetworkingSend_Reliable);
                SendIngameMessage(std::format("Set current map name to \"{}\".", args[2]));
                return;
            }
        }
    }

    if (length >= 3 && length < 512) {
        if (!connected())
            return;
        if (lower1 == "s" || lower1 == "say") {
            bmmo::chat_msg msg{};
            msg.chat_content = bmmo::message_utils::join_strings(args, 2);
            msg.serialize();

            send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        }
        else if (lower1 == "announce") {
            bmmo::important_notification_msg msg{};
            msg.chat_content = bmmo::message_utils::join_strings(args, 2);
            msg.serialize();

            send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        }
        else if (lower1 == "kick") {
            bmmo::kick_request_msg msg{};
            if (args[2][0] == '#')
                msg.player_id = (HSteamNetConnection) atoll(args[2].substr(1).c_str());
            else
                msg.player_name = args[2];
            if (length > 3) {
                msg.reason = bmmo::message_utils::join_strings(args, 3);
            }

            msg.serialize();
            send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        }
        else if (length >= 4 && (lower1 == "whisper" || lower1 == "w")) {
            bmmo::private_chat_msg msg{};
            if (args[2][0] == '#')
                msg.player_id = (HSteamNetConnection) atoll(args[2].substr(1).c_str());
            else
                msg.player_id = (args[2] == db_.get_nickname()) ? db_.get_client_id() : db_.get_client_id(args[2]);
            msg.chat_content = bmmo::message_utils::join_strings(args, 3);
            msg.serialize();
            send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            SendIngameMessage(std::format("Whispered to {}: {}",
                get_username(msg.player_id), msg.chat_content));
        }
        return;
    }

    help(bml);
}

const std::vector<std::string> BallanceMMOClient::OnTabComplete(IBML* bml, const std::vector<std::string>& args) {
    const size_t length = args.size();
    std::string lower1;
    if (length > 1) lower1 = boost::algorithm::to_lower_copy(args[1]);

    switch (length) {
        case 2: {
            return { "connect", "disconnect", "help", "say", "list", "list-id", "cheat", "dnf", "show", "hide", "rank reset", "getmap", "getpos", "announcemap", "teleport", "whisper", "reload", "countdown", "ready", "ready-cancel", "announce" };
            break;
        }
        case 3: {
            if (lower1 == "teleport" || lower1 == "kick" || lower1 == "tp" || lower1 == "whisper" || lower1 == "w") {
                std::vector<std::string> options;
                options.reserve(2 * db_.player_count() + 2);
                db_.for_each([this, &options, &args](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
                    if (pair.first == db_.get_client_id()) return true;
                    options.push_back('#' + std::to_string(pair.first));
                    options.push_back(pair.second.name);
                    return true;
                });
                options.push_back('#' + std::to_string(db_.get_client_id()));
                options.push_back(db_.get_nickname());
                return options;
            }
            break;
        }
        default:
            break;
    }
    return std::vector<std::string>{};
}

void BallanceMMOClient::OnTrafo(int from, int to)
{
    local_state_handler_->poll_and_send_state_forced(player_ball_);
    //throw std::runtime_error("On trafo");
}

void BallanceMMOClient::OnPeerTrafo(uint64_t id, int from, int to)
{
    GetLogger()->Info("OnPeerTrafo, %d -> %d", from, to);
    /*PeerState& peer = peer_[id];
    peer.current_ball = to;
    peer.balls[from]->Show(CKHIDE);
    peer.balls[to]->Show(CKSHOW);*/
}

void BallanceMMOClient::terminate(long delay) {
    static_cast<BallanceMMOClient*>(this_instance_)->SendIngameMessage(
        std::format("Nuking process in {} seconds...", delay).c_str());
    std::this_thread::sleep_for(std::chrono::seconds(delay));

    std::terminate();
}

void BallanceMMOClient::connect_to_server(std::string address) {
    if (connected()) {
        std::lock_guard<std::mutex> lk(bml_mtx_);
        SendIngameMessage("Already connected.");
    }
    else if (connecting()) {
        std::lock_guard<std::mutex> lk(bml_mtx_);
        SendIngameMessage("Connecting in process, please wait...");
    }
    else {
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
        if (address.empty())
            address = props_["remote_addr"]->GetString();
        server_addr = address;
        const auto& [host, port] = bmmo::hostname_parser(address).get_host_components();
        resolver_ = std::make_unique<asio::ip::udp::resolver>(io_ctx_);
        resolver_->async_resolve(host, port, [this](asio::error_code ec, asio::ip::udp::resolver::results_type results) {
            resolving_endpoint_ = false;
            std::lock_guard<std::mutex> lk(bml_mtx_);
            // If address correctly resolved...
            if (!ec) {
                SendIngameMessage("Server address resolved.");

                for (const auto& i : results) {
                    auto endpoint = i.endpoint();
                    std::string connection_string = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
                    GetLogger()->Info("Trying %s", connection_string.c_str());
                    if (connect(connection_string)) {
                        SendIngameMessage("Connecting...");
                        if (network_thread_.joinable())
                            network_thread_.join();
                        network_thread_ = std::thread([this]() { run(); });
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
            GetLogger()->Error(ec.message().c_str());
            work_guard_.reset();
            io_ctx_.stop();
        });
    }
}

void BallanceMMOClient::disconnect_from_server() {
    if (!connecting() && !connected()) {
        std::lock_guard<std::mutex> lk(bml_mtx_);
        SendIngameMessage("Already disconnected.");
    }
    else {
        //client_.disconnect();
        //ping_->update("");
        //status_->update("Disconnected");
        //status_->paint(0xffff0000);
        cleanup();
        std::lock_guard<std::mutex> lk(bml_mtx_);
        SendIngameMessage("Disconnected.");

        ping_->update("");
        status_->update("Disconnected");
        status_->paint(0xffff0000);
    }
}

void BallanceMMOClient::on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
    GetLogger()->Info("Connection status changed. %d -> %d", pInfo->m_eOldState, pInfo->m_info.m_eState);
    estate_ = pInfo->m_info.m_eState;

    switch (pInfo->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_None:
        ping_->update("");
        status_->update("Disconnected");
        status_->paint(0xffff0000);
        break;
    case k_ESteamNetworkingConnectionState_ClosedByPeer: {
        std::string s = std::format("Reason: {} ({})", pInfo->m_info.m_szEndDebug, pInfo->m_info.m_eEndReason);
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
            // Note: we could distinguish between a timeout, a rejected connection,
            // or some other transport problem.
            SendIngameMessage("Connect failed. (ClosedByPeer)");
            GetLogger()->Warn(pInfo->m_info.m_szEndDebug);
            break;
        }
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected) {
            SendIngameMessage("You've been disconnected from the server.");
        }
        SendIngameMessage(s.c_str());
        cleanup();
        int nReason = pInfo->m_info.m_eEndReason - k_ESteamNetConnectionEnd_App_Min;
        // 102 - crash; 103 - fatal error
        if (nReason == 102 || nReason == 103)
            terminate(5);
        else if (nReason >= 150 && nReason < 200) {
            asio::post(thread_pool_, [this, nReason]() {
                SendIngameMessage(std::format("Attempting to reconnect in {}s ...", nReason - 150));
                std::this_thread::sleep_for(std::chrono::seconds(nReason - 150));
                if (!connecting() && !connected())
                    connect_to_server(server_addr);
            });
        }
        break;
    }
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
        std::string s = std::format("Reason: {} ({})", pInfo->m_info.m_szEndDebug, pInfo->m_info.m_eEndReason);
        ping_->update("");
        status_->update("Disconnected");
        status_->paint(0xffff0000);
        // Print an appropriate message
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
            // Note: we could distinguish between a timeout, a rejected connection,
            // or some other transport problem.
            SendIngameMessage("Connect failed. (ProblemDetectedLocally)");
            GetLogger()->Warn(pInfo->m_info.m_szEndDebug);
        }
        else {
            // NOTE: We could check the reason code for a normal disconnection
            SendIngameMessage("Connect failed. (UnknownError)");
            GetLogger()->Warn("Unknown error. (%d->%d) %s", pInfo->m_eOldState, pInfo->m_info.m_eState, pInfo->m_info.m_szEndDebug);
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
        break;
    }

    case k_ESteamNetworkingConnectionState_Connecting:
        // We will get this callback when we start connecting.
        status_->update("Connecting");
        status_->paint(0xFFF6A71B);
        break;

    case k_ESteamNetworkingConnectionState_Connected: {
        status_->update("Connected (Login requested)");
        //status_->paint(0xff00ff00);
        SendIngameMessage("Connected to server.");
        std::string nickname = props_["playername"]->GetString();
        spectator_mode_ = props_["spectator"]->GetBoolean();
        if (spectator_mode_) {
            nickname = bmmo::name_validator::get_spectator_nickname(nickname);
            SendIngameMessage("Note: Spectator Mode is enabled. Your actions will be invisible to other players.");
            local_state_handler_ = std::make_unique<spectator_state_handler>(thread_pool_, this, GetLogger());
            spectator_label_ = std::make_shared<text_sprite>("Spectator_Label", "[Spectator Mode]", RIGHT_MOST, 0.96f);
            spectator_label_->sprite_->SetFont("Arial", get_display_font_size(12), 500, false, false);
            spectator_label_->set_visible(true);
        }
        else {
            local_state_handler_ = std::make_unique<player_state_handler>(thread_pool_, this, GetLogger());
        }
        player_ball_ = get_current_ball();
        if (player_ball_)
            local_state_handler_->set_ball_type(db_.get_ball_id(player_ball_->GetName()));

        SendIngameMessage(("Logging in as \"" + nickname + "\"...").c_str());
        bmmo::login_request_v3_msg msg{};
        db_.set_nickname(nickname);
        msg.nickname = nickname;
        msg.version = version;
        msg.cheated = m_bml->IsCheatEnabled() && !spectator_mode_; // always false in spectator mode
        memcpy(msg.uuid, &uuid_, 16);
        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        if (ping_thread_.joinable())
            ping_thread_.join();
        ping_thread_ = std::thread([this]() {
            do {
                auto status = get_status();
                std::string str = pretty_status(status);
                ping_->update(str, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            } while (connected()); // here's a possible race condition
        });
        break;
    }
    default:
        // Silences -Wswitch
        break;
    }
}

void BallanceMMOClient::on_message(ISteamNetworkingMessage* network_msg) {
    auto* raw_msg = reinterpret_cast<bmmo::general_message*>(network_msg->m_pData);

    switch (raw_msg->code) {
    case bmmo::OwnedBallState: {
        assert(network_msg->m_cbSize == sizeof(bmmo::owned_ball_state_msg));
        auto* obs = reinterpret_cast<bmmo::owned_ball_state_msg*>(network_msg->m_pData);
        bool success = db_.update(obs->content.player_id, TimedBallState(obs->content.state));
        //assert(success);
        if (!success) {
            GetLogger()->Warn("Update db failed: Cannot find such ConnectionID %u. (on_message - OwnedBallState)", obs->content.player_id);
        }
        /*auto state = db_.get(obs->content.player_id);
        GetLogger()->Info("%s: %d, (%.2lf, %.2lf, %.2lf), (%.2lf, %.2lf, %.2lf, %.2lf)",
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
                GetLogger()->Warn("Update db failed: Cannot find such ConnectionID %u. (on_message - OwnedBallState)", i.player_id);
            }
        }
        break;
    }
    case bmmo::OwnedTimedBallState: {
        bmmo::owned_timed_ball_state_msg msg;
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        for (const auto& i : msg.balls) {
            if (!db_.update(i.player_id, TimedBallState(i.state)) && i.player_id != db_.get_client_id()) {
                GetLogger()->Warn("Update db failed: Cannot find such ConnectionID %u. (on_message - OwnedBallState)", i.player_id);
            }
        }
        for (const auto& i : msg.unchanged_balls) {
            if (!db_.update(i.player_id, i.timestamp) && i.player_id != db_.get_client_id()) {
                GetLogger()->Warn("Update db failed: Cannot find such ConnectionID %u. (on_message - OwnedBallState)", i.player_id);
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
            GetLogger()->Error("Deserialize failed!");
        }
        GetLogger()->Info("Online players: ");

        for (auto& i : msg.online_players) {
            if (i.second == db_.get_nickname()) {
                db_.set_client_id(i.first);
            } else {
                db_.create(i.first, i.second);
            }
            GetLogger()->Info(i.second.c_str());
        }*/
        GetLogger()->Warn("Outdated LoginAccepted%s msg received!", (raw_msg->code == bmmo::LoginAcceptedV2) ? "V2" : "");
        break;
    }
    case bmmo::LoginAcceptedV3: {
        auto msg = bmmo::message_utils::deserialize<bmmo::login_accepted_v3_msg>(network_msg);

        if (logged_in_) {
            GetLogger()->Info("New LoginAccepted message received. Resetting current data.");
            db_.clear();
            objects_.destroy_all_objects();
        }
        GetLogger()->Info("%d player(s) online: ", msg.online_players.size());
        for (auto& i : msg.online_players) {
            if (i.name == db_.get_nickname()) {
                db_.set_client_id(i.player_id);
            } else {
                db_.create(i.player_id, i.name, i.cheated);
                db_.update_map(i.player_id, i.map.get_display_name(map_names_), i.sector);
            }
            GetLogger()->Info(i.name.c_str());
        }

        if (logged_in_) {
            asio::post(thread_pool_, [this] {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                db_.reset_time_data();
            });
            break;
        }
        logged_in_ = true;
        status_->update("Connected");
        status_->paint(0xff00ff00);
        SendIngameMessage("Logged in.");

        // post-connection actions
        player_ball_ = get_current_ball();
        if (m_bml->IsIngame() && player_ball_ != nullptr) {
            local_state_handler_->poll_and_send_state_forced(player_ball_);
        }
        if (!current_map_.name.empty()) {
            send_current_map_name();
            map_names_[current_map_.get_hash_bytes_string()] = current_map_.name;
        }
        send_current_map();

        if (spectator_mode_)
            break;

        const auto count = m_bml->GetModCount();
        bmmo::mod_list_msg mod_msg{};
        mod_msg.mods.reserve(count);
        for (auto i = 1; i < count; i++) {
            auto* mod = m_bml->GetMod(i);
            mod_msg.mods.emplace(mod->GetID(), mod->GetVersion());
        }
        mod_msg.serialize();
        send(mod_msg.raw.str().data(), mod_msg.size(), k_nSteamNetworkingSend_Reliable);

        bmmo::hash_data_msg hash_msg{};
        hash_msg.data_name = "Balls.nmo";
        memcpy(hash_msg.md5, balls_nmo_md5_, 16);
        hash_msg.serialize();
        send(hash_msg.raw.str().data(), hash_msg.size(), k_nSteamNetworkingSend_Reliable);
        break;
    }
    case bmmo::PlayerConnected: {
        //bmmo::player_connected_msg msg;
        //msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        //msg.deserialize();
        //SendIngameMessage((msg.name + " joined the game.").c_str());
        //if (m_bml->IsIngame()) {
        //    GetLogger()->Info("Creating game objects for %u, %s", msg.connection_id, msg.name.c_str());
        //    objects_.init_player(msg.connection_id, msg.name);
        //}

        //GetLogger()->Info("Creating state entry for %u, %s", msg.connection_id, msg.name.c_str());
        //db_.create(msg.connection_id, msg.name);

        //// TODO: call this when the player enters a map
        GetLogger()->Warn("Outdated PlayerConnected msg received!");
        
        break;
    }
    case bmmo::PlayerConnectedV2: {
        bmmo::player_connected_v2_msg msg;
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        SendIngameMessage(std::format("{} joined the game with cheat [{}].", msg.name, msg.cheated ? "on" : "off").c_str());
        if (m_bml->IsIngame()) {
            GetLogger()->Info("Creating game objects for %u, %s", msg.connection_id, msg.name.c_str());
            objects_.init_player(msg.connection_id, msg.name, msg.cheated);
        }

        GetLogger()->Info("Creating state entry for %u, %s", msg.connection_id, msg.name.c_str());
        db_.create(msg.connection_id, msg.name, msg.cheated);

        // TODO: call this when the player enters a map

        break;
    }
    case bmmo::PlayerDisconnected: {
        auto* msg = reinterpret_cast<bmmo::player_disconnected_msg *>(network_msg->m_pData);
        auto state = db_.get(msg->content.connection_id);
        //assert(state.has_value());
        if (state.has_value()) {
            SendIngameMessage((state->name + " left the game.").c_str());
            db_.remove(msg->content.connection_id);
            objects_.remove(msg->content.connection_id);
        }
        break;
    }
    case bmmo::Chat: {
        bmmo::chat_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        SendIngameMessage(std::format("{}: {}", get_username(msg.player_id), msg.chat_content).c_str());
        break;
    }
    case bmmo::PrivateChat: {
        bmmo::private_chat_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        SendIngameMessage(std::format("{} whispers to you: {}",
                                      get_username(msg.player_id), msg.chat_content));
        break;
    }
    case bmmo::ImportantNotification: {
        bmmo::important_notification_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        std::string name = get_username(msg.player_id);
        SendIngameMessage(std::format("[Announcement] {}: {}", name, msg.chat_content));
        asio::post(thread_pool_, [this, name, wtext = bmmo::message_utils::ConvertAnsiToWide(msg.chat_content)]() mutable {
            std::string text;
            constexpr static size_t MAX_LINE_LENGTH = 22;
            int line_count;
            for (line_count = 0; wtext.length() > MAX_LINE_LENGTH; ++line_count) {
                text += bmmo::message_utils::ConvertWideToANSI(wtext.substr(0, MAX_LINE_LENGTH)) + '\n';
                wtext.erase(0, MAX_LINE_LENGTH);
            };
            text += bmmo::message_utils::ConvertWideToANSI(wtext) + "\n\n[" + name + "]";
            auto current_second = (SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_) / 1000000;
            text_sprite notification(std::format("Notification{}_{}",
                                     current_second, rand() % 1000), text, 0.0f, 0.4f - 0.02f * line_count);
            notification.sprite_->SetAlignment(CKSPRITETEXT_CENTER);
            notification.sprite_->SetZOrder(65536 + static_cast<int>(current_second));
            notification.sprite_->SetSize({1.0f, 0.2f + 0.08f * line_count});
            notification.sprite_->SetFont(system_font_, get_display_font_size(19), 700, false, false);
            notification.set_visible(true);
            for (int i = 1; i < 15; ++i) {
              notification.paint(0x11FF1190 + i * 0x11001001);
              std::this_thread::sleep_for(std::chrono::milliseconds(44));
            }
            std::this_thread::sleep_for(std::chrono::seconds(9));
            for (int i = 1; i < 15; ++i) {
                notification.paint(0xFFFFF19E - i * 0x11100000);
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
        });
        break;
    }
    case bmmo::PlayerReady: {
        auto* msg = reinterpret_cast<bmmo::player_ready_msg*>(network_msg->m_pData);
        SendIngameMessage(std::format("{} is{} ready to start ({} player{} ready).",
                          get_username(msg->content.player_id),
                          msg->content.ready ? "" : " not",
                          msg->content.count, msg->content.count == 1 ? "" : "s"));
        break;
    }
    case bmmo::Countdown: {
        auto* msg = reinterpret_cast<bmmo::countdown_msg*>(network_msg->m_pData);
        std::string sender_name = get_username(msg->content.sender),
                    map_name = msg->content.map.get_display_name(map_names_);

        switch (msg->content.type) {
            case bmmo::countdown_type::Go: {
                SendIngameMessage(std::format("[{}]: {} - Go!", sender_name, map_name).c_str());
                if ((!msg->content.force_restart && msg->content.map != current_map_) || !m_bml->IsIngame() || spectator_mode_)
                    break;
                if (msg->content.restart_level) {
                    countdown_restart_ = true;
                    restart_current_level();
                }
                else {
                    auto* array_energy = m_bml->GetArrayByName("Energy");
                    int points = 999, lives = 3;
                    if (current_map_.level == 1) points = 1000;
                    array_energy->SetElementValue(0, 0, &points);
                    array_energy->SetElementValue(0, 1, &lives);
                }
                level_start_timestamp_[current_map_.get_hash_bytes_string()] = m_bml->GetTimeManager()->GetTime();
                break;
            }
            case bmmo::countdown_type::Countdown_1:
            case bmmo::countdown_type::Countdown_2:
            case bmmo::countdown_type::Countdown_3:
                SendIngameMessage(std::format("[{}]: {} - {}", sender_name, map_name, (int)msg->content.type).c_str());
                break;
            case bmmo::countdown_type::Ready:
                SendIngameMessage(std::format("[{}]: {} - Get ready", sender_name, map_name).c_str());
                break;
            case bmmo::countdown_type::ConfirmReady:
                SendIngameMessage(std::format("[{}]: {} - Please use \"/mmo ready\" to confirm if you are ready", sender_name, map_name).c_str());
                break;
            case bmmo::countdown_type::Unknown:
            default:
                return;
        }
        break;
    }
    case bmmo::DidNotFinish: {
        auto* msg = reinterpret_cast<bmmo::did_not_finish_msg*>(network_msg->m_pData);
        SendIngameMessage(std::format(
            "{}{} did not finish {} (aborted at sector {}).",
            msg->content.cheated ? "[CHEAT] " : "",
            get_username(msg->content.player_id),
            msg->content.map.get_display_name(map_names_),
            msg->content.sector
        ).c_str());
        break;
    }
    case bmmo::LevelFinishV2: {
        auto* msg = reinterpret_cast<bmmo::level_finish_v2_msg*>(network_msg->m_pData);

        // Prepare data...
        int score = msg->content.levelBonus + msg->content.points + msg->content.lives * msg->content.lifeBonus;

        int total = int(msg->content.timeElapsed);
        int minutes = total / 60;
        int seconds = total % 60;
        int hours = minutes / 60;
        minutes = minutes % 60;
        int ms = int((msg->content.timeElapsed - total) * 1000);

        // Prepare message
        std::string map_name = msg->content.map.get_display_name(map_names_);
        //auto state = db_.get(msg->content.player_id);
        //assert(state.has_value() || (db_.get_client_id() == msg->content.player_id));
        SendIngameMessage(std::format(
            "{}{} finished {} in {}{} place (score: {}; real time: {:02d}:{:02d}:{:02d}.{:03d}).",
            msg->content.cheated ? "[CHEAT] " : "",
            get_username(msg->content.player_id),
            map_name, msg->content.rank, bmmo::get_ordinal_rank(msg->content.rank),
            score, hours, minutes, seconds, ms).c_str());
        // TODO: Stop displaying objects on finish
        break;
    }
    case bmmo::LevelFinish:
        break;
    case bmmo::MapNames: {
        bmmo::map_names_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        map_names_.insert(msg.maps.begin(), msg.maps.end());
        break;
    }
    case bmmo::OwnedCheatState: {
        assert(network_msg->m_cbSize == sizeof(bmmo::owned_cheat_state_msg));
        auto* ocs = reinterpret_cast<bmmo::owned_cheat_state_msg*>(network_msg->m_pData);
        auto state = db_.get(ocs->content.player_id);
        assert(state.has_value() || (db_.get_client_id() == ocs->content.player_id));
        if (state.has_value()) {
            db_.update(ocs->content.player_id, (bool) ocs->content.state.cheated);
        }

        if (ocs->content.state.notify) {
            std::string s = std::format("{} turned cheat [{}].", state.has_value() ? state->name : db_.get_nickname(), ocs->content.state.cheated ? "on" : "off");
            SendIngameMessage(s.c_str());
        }
        break;
    }
    case bmmo::CheatToggle: {
        auto* msg = reinterpret_cast<bmmo::cheat_toggle_msg*>(network_msg->m_pData);
        bool cheat = msg->content.cheated;
        if (cheat != m_bml->IsCheatEnabled() && !spectator_mode_) {
            notify_cheat_toggle_ = false;
            m_bml->EnableCheat(cheat);
            notify_cheat_toggle_ = true;
        }
        std::string str = std::format("Server toggled cheat [{}] globally!", cheat ? "on" : "off");
        SendIngameMessage(str.c_str());
        break;
    }
    case bmmo::OwnedCheatToggle: {
        auto* msg = reinterpret_cast<bmmo::owned_cheat_toggle_msg*>(network_msg->m_pData);
        std::string player_name = "";
        if (msg->content.player_id == db_.get_client_id())
            player_name = db_.get_nickname();
        else {
            auto player_state = db_.get(msg->content.player_id);
            if (player_state.has_value()) {
                player_name = player_state->name;
            }
        }
        if (player_name != "") {
            bool cheat = msg->content.state.cheated;
            std::string str = std::format("{} toggled cheat [{}] globally!", player_name, cheat ? "on" : "off");
            if (cheat != m_bml->IsCheatEnabled() && !spectator_mode_) {
                notify_cheat_toggle_ = false;
                m_bml->EnableCheat(cheat);
                notify_cheat_toggle_ = true;
            }
            SendIngameMessage(str.c_str());
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
        auto* msg = reinterpret_cast<bmmo::simple_action_msg*>(network_msg->m_pData);
        switch (msg->content.action) {
            case bmmo::action_type::LoginDenied:
                SendIngameMessage("Login denied.");
                break;
            case bmmo::action_type::CurrentMapQuery: {
                break;
            }
            case bmmo::action_type::Unknown:
            default:
                GetLogger()->Error("Unknown action request received.");
        }
        break;
    }
    case bmmo::ActionDenied: {
        auto* msg = reinterpret_cast<bmmo::action_denied_msg*>(network_msg->m_pData);

        using dr = bmmo::deny_reason;
        std::string reason = std::unordered_map<dr, const char*>{
            {dr::NoPermission, "you don't have the permission to run this action."},
            {dr::InvalidAction, "invalid action."},
            {dr::InvalidTarget, "invalid target."},
            {dr::TargetNotFound, "target not found."},
            {dr::PlayerMuted, "you are not allowed to post public messages on this server."},
        }[msg->content.reason];
        if (reason.empty()) reason = "unknown reason.";

        SendIngameMessage(("Action failed: " + reason).c_str());
        break;
    }
    case bmmo::CurrentMap: {
        auto* msg = reinterpret_cast<bmmo::current_map_msg*>(network_msg->m_pData);
        if (msg->content.type == bmmo::current_map_state::Announcement) {
            SendIngameMessage(std::format("{}{} is at the {}{} sector of {}.",
                              (db_.get_client_id() == msg->content.player_id ? m_bml->IsCheatEnabled()
                              : db_.get(msg->content.player_id).value().cheated) ? "[CHEAT] " : "",
                              get_username(msg->content.player_id), msg->content.sector,
                              bmmo::get_ordinal_rank(msg->content.sector),
                              msg->content.map.get_display_name(map_names_)));
        }
        else {
            db_.update_map(msg->content.player_id, msg->content.map.get_display_name(map_names_), msg->content.sector);
        }
        break;
    }
    case bmmo::CurrentSector: {
        auto* msg = reinterpret_cast<bmmo::current_sector_msg*>(network_msg->m_pData);
        db_.update_sector(msg->content.player_id, msg->content.sector);
        break;
    }
    case bmmo::OpState: {
        auto* msg = reinterpret_cast<bmmo::op_state_msg*>(network_msg->m_pData);
        SendIngameMessage(std::format("You have been {} Operator permission.",
                msg->content.op ? "granted" : "removed from").c_str());
        break;
    }
    case bmmo::PlainText: {
        bmmo::plain_text_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        SendIngameMessage(msg.text_content.c_str());
        break;
    }
    case bmmo::PopupBox: {
        auto msg = bmmo::message_utils::deserialize<bmmo::popup_box_msg>(network_msg);
        SendIngameMessage("[Popup] {" + msg.title + "}: " + msg.text_content);
        std::thread([msg = std::move(msg)] {
            int popup = MessageBox(NULL, msg.text_content.c_str(), msg.title.c_str(), MB_OK | MB_ICONINFORMATION);
        }).detach();
        break;
    }
    default:
        GetLogger()->Error("Invalid message with opcode %d received.", raw_msg->code);
        break;
    }
}

void BallanceMMOClient::LoggingOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg)
{
    static const char* fmt_string = "[%d] %10.6f %s";
    SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
    switch (eType) {
        case k_ESteamNetworkingSocketsDebugOutputType_Bug:
        case k_ESteamNetworkingSocketsDebugOutputType_Error:
            static_cast<BallanceMMOClient*>(this_instance_)->GetLogger()->Error(fmt_string, eType, time * 1e-6, pszMsg);
            break;
        case k_ESteamNetworkingSocketsDebugOutputType_Important:
        case k_ESteamNetworkingSocketsDebugOutputType_Warning:
            static_cast<BallanceMMOClient*>(this_instance_)->GetLogger()->Warn(fmt_string, eType, time * 1e-6, pszMsg);
            break;
        default:
            static_cast<BallanceMMOClient*>(this_instance_)->GetLogger()->Info(fmt_string, eType, time * 1e-6, pszMsg);
    }

    if (eType == k_ESteamNetworkingSocketsDebugOutputType_Bug) {
        static_cast<BallanceMMOClient*>(this_instance_)->SendIngameMessage("BallanceMMO has encountered a bug which is fatal. Please contact developer with this piece of log.");
        terminate(5);
    }
}
