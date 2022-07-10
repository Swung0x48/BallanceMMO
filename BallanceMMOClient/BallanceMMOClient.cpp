#include "BallanceMMOClient.h"
#include <io.h>
#include <fcntl.h>

IMod* BMLEntry(IBML* bml) {
    DeclareDumpFile();
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

void BallanceMMOClient::OnLoad()
{
    init_config();
    //client_ = std::make_unique<client>(GetLogger(), m_bml);
    input_manager = m_bml->GetInputManager();
    if (AllocConsole()) {
        SetConsoleTitle("BallanceMMO Console");
        BindCrtHandlesToStdHandles(true, true, true);
/*        freopen("CONIN$", "r", stdin);
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);*/
        console_thread_ = std::thread([&]() {
            while (console_running_) {
                std::cout << "\r> " << std::flush;
                std::wstring wline;
                _setmode(_fileno(stdin), _O_U16TEXT);
                std::getline(std::wcin, wline);
                // std::wcout << wline << std::endl;
                std::string line = bmmo::message_utils::ConvertWideToANSI(wline),
                            cmd = "ballancemmo";
                line.erase(line.rfind('\r'));
                std::vector<std::string> args;
                bmmo::command_parser parser(line);
                while (!cmd.empty()) {
                    args.push_back(cmd);
                    cmd = parser.get_next_word();
                }
                OnCommand(m_bml, args);
            }
        });
    };
}

void BallanceMMOClient::OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj)
{
    if (strcmp(filename, "3D Entities\\Balls.nmo") == 0) {
        objects_.destroy_all_objects();
        objects_.init_template_balls();
        //objects_.init_players();
    }

    if (strcmp(filename, "3D Entities\\Gameplay.nmo") == 0) {
        current_level_array_ = CKOBJID(m_bml->GetArrayByName("CurrentLevel"));
    }

    if (isMap) {
        GetLogger()->Info("Initializing peer objects...");
        objects_.init_players();
        boost::regex name_pattern(".*\\\\(Level|Maps)\\\\(.*).nmo", boost::regex::icase);
        std::string path = std::string(filename);
        boost::smatch matched;
        if (boost::regex_search(path, matched, name_pattern)) {
            current_map_.name = matched[2].str();
            if (boost::iequals(matched[1].str(), "Maps")) {
                current_map_.type = bmmo::CustomMap;
            }
            else {
                current_map_.type = bmmo::OriginalLevel;
                path = "..\\" + path;
            }
        } else {
            current_map_.name = std::string(filename);
            current_map_.type = bmmo::UnknownType;
        }
        md5_from_file(path, current_map_.md5);
        map_names_[current_map_.get_hash_bytes_string()] = current_map_.name;
        GetLogger()->Info("Current map: %s; type: %d; md5: %s.",
            current_map_.name.c_str(), (int)current_map_.type, current_map_.get_hash_string().c_str());
        reset_timer_ = true;
        if (connected()) {
            send_current_map_name();
        };
        GetLogger()->Info("Initialization completed.");
    }

    /*if (isMap) {
        std::string filename_string(filename);
        std::filesystem::path path = std::filesystem::current_path().parent_path().append(filename_string[0] == '.' ? filename_string.substr(3, filename_string.length()) : filename_string);
        std::ifstream map(path, std::ios::in | std::ios::binary);
        map_hash_ = hash_sha256(map);
        m_bml_SendIngameMessage(map_hash_.c_str());
        blcl::net::message<MsgType> msg;
        msg.header.id = MsgType::EnterMap;
        client_.send(msg);
    }*/
}

void BallanceMMOClient::OnPostStartMenu()
{
    if (init_) {
        GetLogger()->Info("Destroying peer objects...");
        objects_.destroy_all_objects();
        GetLogger()->Info("Destroy completed.");
    }

    if (!init_) {
        ping_ = std::make_shared<text_sprite>("T_MMO_PING", "", RIGHT_MOST, 0.03f);
        ping_->sprite_->SetSize(Vx2DVector(RIGHT_MOST, 0.4f));
        status_ = std::make_shared<text_sprite>("T_MMO_STATUS", "Disconnected", RIGHT_MOST, 0.0f);
        status_->paint(0xffff0000);
        
        m_bml->RegisterCommand(new CommandMMO([this](IBML* bml, const std::vector<std::string>& args) { OnCommand(bml, args); }));
        m_bml->RegisterCommand(new CommandMMOSay([this](IBML* bml, const std::vector<std::string>& args) { OnCommand(bml, args); }));
        init_ = true;
    }
}

void BallanceMMOClient::OnProcess() {
    //poll_connection_state_changes();
    //poll_incoming_messages();

    //poll_status_toggle();
    poll_local_input();

    if (!connected())
        return;

    std::unique_lock<std::mutex> bml_lk(bml_mtx_, std::try_to_lock);

    if (bml_lk) {
        if (m_bml->IsIngame()) {
            auto ball = get_current_ball();
            if (player_ball_ == nullptr)
                player_ball_ = ball;

            check_on_trafo(ball);
            poll_player_ball_state();

            const auto current_timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
            if (current_timestamp >= next_update_timestamp_) {
                if (current_timestamp - next_update_timestamp_ > 1000000)
                    next_update_timestamp_ = current_timestamp;
                next_update_timestamp_ += MINIMUM_UPDATE_INTERVAL;

                asio::post(thread_pool_, [this]() {
                    assemble_and_send_state();
                });
            }

            objects_.update(db_.flush());
        }
    }
}

void BallanceMMOClient::OnStartLevel()
{
    /*if (!connected())
        return;*/

    player_ball_ = get_current_ball();
    local_ball_state_.type = db_.get_ball_id(player_ball_->GetName()); 

    if (reset_timer_) {
        level_start_timestamp_[current_map_.get_hash_bytes_string()] = m_bml->GetTimeManager()->GetTime();
        reset_timer_ = false;
    }

    m_bml->GetArrayByName("CurrentLevel")->GetElementValue(0, 0, &current_map_.level);

    //objects_.destroy_all_objects();
    //objects_.init_players();
}

void BallanceMMOClient::OnLevelFinish() {
    bmmo::level_finish_v2_msg msg{};
    auto* array_energy = m_bml->GetArrayByName("Energy");
    array_energy->GetElementValue(0, 0, &msg.content.points);
    array_energy->GetElementValue(0, 1, &msg.content.lives);
    array_energy->GetElementValue(0, 5, &msg.content.lifeBonus);
    m_bml->GetArrayByName("CurrentLevel")->GetElementValue(0, 0, &current_map_.level);
    m_bml->GetArrayByName("AllLevel")->GetElementValue(current_map_.level - 1, 6, &msg.content.levelBonus);
    msg.content.timeElapsed = (m_bml->GetTimeManager()->GetTime() - level_start_timestamp_[current_map_.get_hash_bytes_string()]) / 1e3;
    reset_timer_ = true;
    msg.content.cheated = m_bml->IsCheatEnabled();
    msg.content.map = current_map_;
    GetLogger()->Info("Sending level finish message...");

    send(msg, k_nSteamNetworkingSend_Reliable);
}

void BallanceMMOClient::OnLoadScript(CKSTRING filename, CKBehavior* script)
{
    if (strcmp(script->GetName(), "Gameplay_Ingame") == 0)
        edit_Gameplay_Ingame(script);
    if (strcmp(script->GetName(), "Gameplay_Events") == 0)
        edit_Gameplay_Events(script);
    if (strcmp(script->GetName(), "Gameplay_Energy") == 0)
        edit_Gameplay_Energy(script);
    if (strcmp(script->GetName(), "Event_handler") == 0)
        edit_Event_handler(script);
    if (strcmp(script->GetName(), "Menu_Pause") == 0)
        edit_Menu_Pause(script);
}

void BallanceMMOClient::OnCheatEnabled(bool enable) {
    bmmo::cheat_state_msg msg{};
    msg.content.cheated = enable;
    msg.content.notify = notify_cheat_toggle_;
    send(msg, k_nSteamNetworkingSend_Reliable);
}

void BallanceMMOClient::OnModifyConfig(CKSTRING category, CKSTRING key, IProperty* prop) {
    if (prop == props_["playername"]) {
        validate_nickname(prop);
    } else if (prop == props_["uuid"]) {
        prop->SetString(boost::uuids::to_string(uuid_).c_str());
        GetLogger()->Warn("Warning: Unable to modify UUID.");
    }
}

void BallanceMMOClient::OnExitGame()
{
    cleanup(true);
}

void BallanceMMOClient::OnUnload() {
    cleanup(true);
    client::destroy();
}

void BallanceMMOClient::OnCommand(IBML* bml, const std::vector<std::string>& args)
{
    auto help = [this](IBML* bml) {
        std::lock_guard<std::mutex> lk(bml_mtx_);
        bml_SendIngameMessage("BallanceMMO Help");
        bml_SendIngameMessage("/mmo connect - Connect to server.");
        bml_SendIngameMessage("/mmo disconnect - Disconnect from server.");
        bml_SendIngameMessage("/mmo list - List online players.");
        bml_SendIngameMessage("/mmo say - Send message to each other.");
    };

    const size_t length = args.size();

    switch (length) {
        case 1: {
            help(bml);
            return;
        }
        case 2: {
            if (args[1] == "connect" || args[1] == "c") {
                if (connected()) {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml_SendIngameMessage("Already connected.");
                }
                else if (connecting()) {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml_SendIngameMessage("Connecting in process, please wait...");
                }
                else {
                    bml_SendIngameMessage("Resolving server address...");
                    resolving_endpoint_ = true;
                    // Bootstrap io_context
                    work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(io_ctx_.get_executor());
                    asio::post(thread_pool_, [this]() {
                        if (io_ctx_.stopped())
                            io_ctx_.reset();
                        io_ctx_.run();
                        });

                    // Resolve address
                    auto p = bmmo::hostname_parser(props_["remote_addr"]->GetString()).get_host_components();
                    resolver_ = std::make_unique<asio::ip::udp::resolver>(io_ctx_);
                    resolver_->async_resolve(p.first, p.second, [this, bml](asio::error_code ec, asio::ip::udp::resolver::results_type results) {
                        resolving_endpoint_ = false;
                        std::lock_guard<std::mutex> lk(bml_mtx_);
                        // If address correctly resolved...
                        if (!ec) {
                            bml_SendIngameMessage("Server address resolved.");
                            
                            for (const auto& i : results) {
                                auto endpoint = i.endpoint();
                                std::string connection_string = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
                                GetLogger()->Info("Trying %s", connection_string.c_str());
                                if (connect(connection_string)) {
                                    bml_SendIngameMessage("Connecting...");
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
                            bml_SendIngameMessage("Failed to connect to server. All resolved address appears to be unresolvable.");
                            work_guard_.reset();
                            io_ctx_.stop();
                            resolver_.reset();
                            return;
                        }
                        // If not correctly resolved...
                        bml_SendIngameMessage("Failed to resolve hostname.");
                        GetLogger()->Error(ec.message().c_str());
                        work_guard_.reset();
                        io_ctx_.stop();
                    });
                }
            }
            else if (args[1] == "disconnect" || args[1] == "d") {
                if (!connecting() && !connected()) {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml_SendIngameMessage("Already disconnected.");
                }
                else {
                    //client_.disconnect();
                    //ping_->update("");
                    //status_->update("Disconnected");
                    //status_->paint(0xffff0000);
                    cleanup();
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml_SendIngameMessage("Disconnected.");

                    ping_->update("");
                    status_->update("Disconnected");
                    status_->paint(0xffff0000);
                }
            }
            else if (args[1] == "list" || args[1] == "l" || args[1] == "list-id" || args[1] == "li") {
                if (!connected())
                    return;

                std::string line = "";
                int counter = 0;
                bool show_id = (args[1] == "list-id" || args[1] == "li");
                db_.for_each([this, &line, &counter, &show_id](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
                    ++counter;
                    line.append(pair.second.name + (pair.second.cheated ? " [CHEAT]" : "")
                        + (show_id ? (": " + std::to_string(pair.first)): "") + ", ");
                    if (counter == (show_id ? 2 : 4)) {
                        bml_SendIngameMessage(line.c_str());
                        counter = 0;
                        line = "";
                    }
                    return true;
                });
                line.append(db_.get_nickname() + (m_bml->IsCheatEnabled() ? " [CHEAT]" : "")
                    + (show_id ? (": " + std::to_string(db_.get_client_id())) : ""))
                    .append("   (" + std::to_string(db_.player_count(db_.get_client_id()) + 1) + " total)");
                m_bml_SendIngameMessage(line.c_str());
            }
            else if (args[1] == "dnf") {
                bmmo::did_not_finish_msg msg{};
                if (current_map_.get_hash_string() == bmmo::original_map_hashes[0])
                    return;
                m_bml->GetArrayByName("IngameParameter")->GetElementValue(0, 1, &msg.content.sector);
                msg.content.map = current_map_;
                msg.content.cheated = m_bml->IsCheatEnabled();
                send(msg, k_nSteamNetworkingSend_Reliable);
            }
            /*else if (args[1] == "p") {
                objects_.physicalize_all();
            } else if (args[1] == "f") {
                ExecuteBB::SetPhysicsForce(player_ball_, VxVector(0, 0, 0), player_ball_, VxVector(1, 0, 0), m_bml->Get3dObjectByName("Cam_OrientRef"), .43f);
            } else if (args[1] == "u") {
                ExecuteBB::UnsetPhysicsForce(player_ball_);
            }*/
            return;
        }
        case 3: {
            if (args[1] == "cheat") {
                bool cheat_state = false;
                if (args[2] == "on")
                    cheat_state = true;
                bmmo::cheat_toggle_msg msg{};
                msg.content.cheated = cheat_state;
                send(msg, k_nSteamNetworkingSend_Reliable);
                return;
            }
            if (args[1] == "rank" && args[2] == "reset") {
                reset_rank_ = true;
                return;
            }    
        }
    }

    if (length >= 3 && length < 512) {
        if (!connected())
            return;
        if (args[1] == "s" || args[1] == "say") {
            bmmo::chat_msg msg{};
            try {
                msg.chat_content = join_strings(args, 2);
            }
            catch (const char* s) {
                bml_SendIngameMessage(s);
                return;
            };
            // temporarily disable /mmo s dnf
            if (boost::iequals(msg.chat_content, "dnf")) {
                m_bml_SendIngameMessage("Note: please press Ctrl+D twice to send the DNF message.");
                return;
            }
            /////////////////////////////////
            msg.serialize();

            send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        }
        else if (args[1] == "kick" || args[1] == "kick-id") {
            bmmo::kick_request_msg msg{};
            if (args[1] == "kick")
                msg.player_name = args[2];
            else
                msg.player_id = atoll(args[2].c_str());
            if (length > 3) {
                try {
                    msg.reason = join_strings(args, 3);
                }
                catch (const char* s) {
                    bml_SendIngameMessage(s);
                    return;
                };
            }

            msg.serialize();
            send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        }
        return;
    }

    help(bml);
}

void BallanceMMOClient::OnTrafo(int from, int to)
{
    poll_player_ball_state();
    asio::post(thread_pool_, [this]() {
        assemble_and_send_state();
    });
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
    static_cast<BallanceMMOClient*>(this_instance_)->m_bml_SendIngameMessage(
        std::format("Nuking process in {} seconds...", delay).c_str());
    std::this_thread::sleep_for(std::chrono::seconds(delay));

    std::terminate();
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
        string s = std::format("Reason: {} ({})", pInfo->m_info.m_szEndDebug, pInfo->m_info.m_eEndReason);
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
            // Note: we could distinguish between a timeout, a rejected connection,
            // or some other transport problem.
            m_bml_SendIngameMessage("Connect failed. (ClosedByPeer)");
            GetLogger()->Warn(pInfo->m_info.m_szEndDebug);
            break;
        }
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected) {
            m_bml_SendIngameMessage("You've been disconnected from the server.");
        }
        m_bml_SendIngameMessage(s.c_str());
        cleanup();
        if (pInfo->m_info.m_eEndReason == k_ESteamNetConnectionEnd_App_Min + 102)
            terminate(5);
        break;
    }
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
        string s = std::format("Reason: {} ({})", pInfo->m_info.m_szEndDebug, pInfo->m_info.m_eEndReason);
        ping_->update("");
        status_->update("Disconnected");
        status_->paint(0xffff0000);
        // Print an appropriate message
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
            // Note: we could distinguish between a timeout, a rejected connection,
            // or some other transport problem.
            m_bml_SendIngameMessage("Connect failed. (ProblemDetectedLocally)");
            GetLogger()->Warn(pInfo->m_info.m_szEndDebug);
        }
        else {
            // NOTE: We could check the reason code for a normal disconnection
            m_bml_SendIngameMessage("Connect failed. (UnknownError)");
            GetLogger()->Warn("Unknown error. (%d->%d) %s", pInfo->m_eOldState, pInfo->m_info.m_eState, pInfo->m_info.m_szEndDebug);
        }
        m_bml_SendIngameMessage(s.c_str());
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
        m_bml_SendIngameMessage("Connected to server.");
        m_bml_SendIngameMessage((std::string("Logging in as ") + "\"" + props_["playername"]->GetString() + "\"...").c_str());
        bmmo::login_request_v3_msg msg;
        validate_nickname(props_["playername"]);
        db_.set_nickname(props_["playername"]->GetString());
        msg.nickname = props_["playername"]->GetString();
        msg.version = version;
        msg.cheated = m_bml->IsCheatEnabled();
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
        bool success = db_.update(obs->content.player_id, reinterpret_cast<const BallState&>(obs->content.state));
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
            if (!db_.update(i.player_id, reinterpret_cast<const BallState&>(i.state)) && i.player_id != db_.get_client_id()) {
                GetLogger()->Warn("Update db failed: Cannot find such ConnectionID %u. (on_message - OwnedBallState)", i.player_id);
            }
        }
        break;
    }
    case bmmo::LoginAccepted: {
        /*status_->update("Connected");
        status_->paint(0xff00ff00);
        m_bml_SendIngameMessage("Logged in.");
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
        GetLogger()->Warn("Outdated LoginAccepted msg received!");
        break;
    }
    case bmmo::LoginAcceptedV2: {
        logged_in_ = true;
        status_->update("Connected");
        status_->paint(0xff00ff00);
        m_bml_SendIngameMessage("Logged in.");
        bmmo::login_accepted_v2_msg msg;
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (!msg.deserialize()) {
            GetLogger()->Error("Deserialization failed!");
        }
        GetLogger()->Info((std::to_string(msg.online_players.size()) + " players online: ").c_str());
        
        for (auto& i : msg.online_players) {
            if (i.second.name == db_.get_nickname()) {
                db_.set_client_id(i.first);
            } else {
                db_.create(i.first, i.second.name, i.second.cheated);
            }
            GetLogger()->Info(i.second.name.c_str());
        }

        if (!current_map_.name.empty())
            send_current_map_name();
        break;
    }
    case bmmo::PlayerConnected: {
        //bmmo::player_connected_msg msg;
        //msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        //msg.deserialize();
        //m_bml_SendIngameMessage((msg.name + " joined the game.").c_str());
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
        m_bml_SendIngameMessage(std::format("{} joined the game with cheat [{}].", msg.name, msg.cheated ? "on" : "off").c_str());
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
            m_bml_SendIngameMessage((state->name + " left the game.").c_str());
            db_.remove(msg->content.connection_id);
            objects_.remove(msg->content.connection_id);
        }
        break;
    }
    case bmmo::Chat: {
        bmmo::chat_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        std::string print_msg;

        if (msg.player_id == k_HSteamNetConnection_Invalid)
            print_msg = std::format("[Server]: {}", msg.chat_content);
        else {
            auto state = db_.get(msg.player_id);
            assert(state.has_value() || (db_.get_client_id() == msg.player_id));
            print_msg = std::format("{}: {}", state.has_value() ? state->name : db_.get_nickname(), msg.chat_content);
        }
        m_bml_SendIngameMessage(print_msg.c_str());
        break;
    }
    case bmmo::Countdown: {
        auto* msg = reinterpret_cast<bmmo::countdown_msg*>(network_msg->m_pData);
        std::string sender_name = get_username(msg->content.sender),
                    map_name = msg->content.map.get_display_name(map_names_);

        switch (msg->content.type) {
            case bmmo::CountdownType_Go: {
                m_bml_SendIngameMessage(std::format("[{}]: {} - Go!", sender_name, map_name).c_str());
                if ((!msg->content.force_restart && msg->content.map != current_map_) || !m_bml->IsIngame())
                    break;
                if (msg->content.restart_level) {
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
            case bmmo::CountdownType_1:
            case bmmo::CountdownType_2:
            case bmmo::CountdownType_3:
                m_bml_SendIngameMessage(std::format("[{}]: {} - {}", sender_name, map_name, (int)msg->content.type).c_str());
                break;
            case bmmo::CountdownType_Unknown:
            default:
                return;
        }
        break;
    }
    case bmmo::DidNotFinish: {
        auto* msg = reinterpret_cast<bmmo::did_not_finish_msg*>(network_msg->m_pData);
        m_bml_SendIngameMessage(std::format(
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
        m_bml_SendIngameMessage(std::format(
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
            db_.update(ocs->content.player_id, ocs->content.state.cheated);
        }

        if (ocs->content.state.notify) {
            std::string s = std::format("{} turned cheat [{}].", state.has_value() ? state->name : db_.get_nickname(), ocs->content.state.cheated ? "on" : "off");
            m_bml_SendIngameMessage(s.c_str());
        }
        break;
    }
    case bmmo::CheatToggle: {
        auto* msg = reinterpret_cast<bmmo::cheat_toggle_msg*>(network_msg->m_pData);
        bool cheat = msg->content.cheated;
        notify_cheat_toggle_ = false;
        m_bml->EnableCheat(cheat);
        notify_cheat_toggle_ = true;
        std::string str = std::format("Server toggled cheat [{}] globally!", cheat ? "on" : "off");
        m_bml_SendIngameMessage(str.c_str());
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
            notify_cheat_toggle_ = false;
            m_bml->EnableCheat(cheat);
            notify_cheat_toggle_ = true;
            m_bml_SendIngameMessage(str.c_str());
        }
        break;
    }
    case bmmo::PlayerKicked: {
        bmmo::player_kicked_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        m_bml_SendIngameMessage(std::format("{} was kicked by {}{}{}.",
            msg.kicked_player_name,
            (msg.executor_name == "") ? "the server" : msg.executor_name,
            (msg.reason == "") ? "" : " (" + msg.reason + ")",
            msg.crashed ? " and crashed subsequently" : ""
        ).c_str());

        break;
    }
    case bmmo::ActionDenied: {
        auto* msg = reinterpret_cast<bmmo::action_denied_msg*>(network_msg->m_pData);

        std::string reason;
        switch (msg->content.reason) {
            case bmmo::NoPermission:
                reason = "you don't have the permission to run this action.";
                break;
            case bmmo::InvalidAction:
                reason = "invalid action.";
                break;
            case bmmo::InvalidTarget:
                reason = "invalid target.";
                break;
            case bmmo::TargetNotFound:
                reason = "target not found.";
                break;
            case bmmo::UnknownReason:
            default:
                reason = "unknown reason.";
        }

        m_bml_SendIngameMessage(("Action failed: " + reason).c_str());
        break;
    }
    case bmmo::OpState: {
        auto* msg = reinterpret_cast<bmmo::op_state_msg*>(network_msg->m_pData);
        m_bml_SendIngameMessage(std::format("You have been {} Operator permission.",
                msg->content.op ? "granted" : "removed from").c_str());
        break;
    }
    case bmmo::PlainText: {
        bmmo::plain_text_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        m_bml_SendIngameMessage(msg.text_content.c_str());
        break;
    }
    default:
        GetLogger()->Error("Invalid message with opcode %d received.", raw_msg->code);
        break;
    }
}

void BallanceMMOClient::LoggingOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg)
{
    const char* fmt_string = "[%d] %10.6f %s\n";
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
        static_cast<BallanceMMOClient*>(this_instance_)->m_bml_SendIngameMessage("BallanceMMO has encountered a bug which is fatal. Please contact developer with this piece of log.");
        terminate(5);
    }
}
