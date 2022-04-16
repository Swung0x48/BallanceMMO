#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
    DeclareDumpFile();
    BallanceMMOClient::init_socket();
    return new BallanceMMOClient(bml);
}

void BallanceMMOClient::OnLoad()
{
    init_config();
    //client_ = std::make_unique<client>(GetLogger(), m_bml);
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
        GetLogger()->Info("Initialize completed.");
    }

    /*if (isMap) {
        std::string filename_string(filename);
        std::filesystem::path path = std::filesystem::current_path().parent_path().append(filename_string[0] == '.' ? filename_string.substr(3, filename_string.length()) : filename_string);
        std::ifstream map(path, std::ios::in | std::ios::binary);
        map_hash_ = hash_sha256(map);
        m_bml->SendIngameMessage(map_hash_.c_str());
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

            asio::post(thread_pool_, [this]() {
                assemble_and_send_state();
            });

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

    level_start_timestamp_ = m_bml->GetTimeManager()->GetTime();

    //objects_.destroy_all_objects();
    //objects_.init_players();
}

void BallanceMMOClient::OnLevelFinish() {
    bmmo::level_finish_msg msg{};
    auto* array_energy = m_bml->GetArrayByName("Energy");
    array_energy->GetElementValue(0, 0, &msg.content.points);
    array_energy->GetElementValue(0, 1, &msg.content.lifes);
    array_energy->GetElementValue(0, 5, &msg.content.lifeBouns);
    m_bml->GetArrayByName("CurrentLevel")->GetElementValue(0, 0, &msg.content.currentLevel);
    m_bml->GetArrayByName("AllLevel")->GetElementValue(msg.content.currentLevel - 1, 6, &msg.content.levelBouns);
    msg.content.timeElapsed = (m_bml->GetTimeManager()->GetTime() - level_start_timestamp_) / 1e3;
    GetLogger()->Info("Sending level finish message...");
    send(msg, k_nSteamNetworkingSend_Reliable);
}

void BallanceMMOClient::OnLoadScript(CKSTRING filename, CKBehavior* script)
{
    if (strcmp(script->GetName(), "Gameplay_Ingame") == 0)
        edit_Gameplay_Ingame(script);
    if (strcmp(script->GetName(), "Gameplay_Events") == 0)
        edit_Gameplay_Events(script);
    if (strcmp(script->GetName(), "Event_handler") == 0)
        edit_Event_handler(script);
}

void BallanceMMOClient::OnCheatEnabled(bool enable) {
    bmmo::cheat_state_msg msg{};
    msg.content.cheated = enable;
    msg.content.notify = notify_cheat_toggle_;
    send(msg, k_nSteamNetworkingSend_Reliable);
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
        bml->SendIngameMessage("BallanceMMO Help");
        bml->SendIngameMessage("/mmo connect - Connect to server.");
        bml->SendIngameMessage("/mmo disconnect - Disconnect from server.");
        bml->SendIngameMessage("/mmo list - List online players.");
        bml->SendIngameMessage("/mmo say - Send message to each other.");
    };

    switch (args.size()) {
        case 1: {
            help(bml);
            break;
        }
        case 2: {
            if (args[1] == "connect" || args[1] == "c") {
                if (connected()) {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml->SendIngameMessage("Already connected.");
                }
                else if (connecting()) {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml->SendIngameMessage("Connecting in process, please wait...");
                }
                else {
                    bml->SendIngameMessage("Resolving server address...");
                    resolving_endpoint_ = true;
                    // Bootstrap io_context
                    work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(io_ctx_.get_executor());
                    asio::post(thread_pool_, [this]() {
                        if (io_ctx_.stopped())
                            io_ctx_.reset();
                        io_ctx_.run();
                        });

                    // Resolve address
                    auto p = parse_connection_string(props_["remote_addr"]->GetString());
                    resolver_ = std::make_unique<asio::ip::udp::resolver>(io_ctx_);
                    resolver_->async_resolve(p.first, p.second, [this, bml](asio::error_code ec, asio::ip::udp::resolver::results_type results) {
                        resolving_endpoint_ = false;
                        std::lock_guard<std::mutex> lk(bml_mtx_);
                        // If address correctly resolved...
                        if (!ec) {
                            bml->SendIngameMessage("Server address resolved.");
                            
                            for (const auto& i : results) {
                                auto endpoint = i.endpoint();
                                std::string connection_string = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
                                GetLogger()->Info("Trying %s", connection_string.c_str());
                                if (connect(connection_string)) {
                                    bml->SendIngameMessage("Connecting...");
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
                            bml->SendIngameMessage("Connect to server failed. All resolved address appears to be unresolvable.");
                            work_guard_.reset();
                            io_ctx_.stop();
                            resolver_.reset();
                            return;
                        }
                        // If not correctly resolved...
                        bml->SendIngameMessage("Failed to resolve hostname.");
                        GetLogger()->Error(ec.message().c_str());
                        work_guard_.reset();
                        io_ctx_.stop();
                        return;
                    });
                }
            }
            else if (args[1] == "disconnect" || args[1] == "d") {
                if (!connecting() && !connected()) {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml->SendIngameMessage("Already disconnected.");
                }
                else {
                    //client_.disconnect();
                    //ping_->update("");
                    //status_->update("Disconnected");
                    //status_->paint(0xffff0000);
                    cleanup();
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml->SendIngameMessage("Disconnected.");

                    ping_->update("");
                    status_->update("Disconnected");
                    status_->paint(0xffff0000);
                }
            }
            else if (args[1] == "list" || args[1] == "l") {
                if (!connected())
                    break;

                std::stringstream ss;
                db_.for_each([&ss](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
                    ss << pair.second.name << (pair.second.cheated ? " [CHEAT]" : "") << ", ";
                    return true;
                });
                ss << db_.get_nickname() << (m_bml->IsCheatEnabled() ? " [CHEAT]" : "");

                m_bml->SendIngameMessage(ss.str().c_str());
            }
            /*else if (args[1] == "p") {
                objects_.physicalize_all();
            } else if (args[1] == "f") {
                ExecuteBB::SetPhysicsForce(player_ball_, VxVector(0, 0, 0), player_ball_, VxVector(1, 0, 0), m_bml->Get3dObjectByName("Cam_OrientRef"), .43f);
            } else if (args[1] == "u") {
                ExecuteBB::UnsetPhysicsForce(player_ball_);
            }*/
            break;
        }
        case 3: {
            if (args[1] == "s" || args[1] == "say") {
                if (!connected())
                    break;

                bmmo::chat_msg msg{};
                msg.chat_content = args[2];
                msg.serialize();

                send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            }
            else if (args[1] == "cheat") {
                bool cheat_state = false;
                if (args[2] == "on")
                    cheat_state = true;
                bmmo::cheat_toggle_msg msg{};
                msg.content.cheated = cheat_state;
                send(msg, k_nSteamNetworkingSend_Reliable);
            }
            break;
        }
        default: {
            help(bml);
        }
    }
}

void BallanceMMOClient::OnTrafo(int from, int to)
{
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
            m_bml->SendIngameMessage("Connect failed. (ClosedByPeer)");
            GetLogger()->Warn(pInfo->m_info.m_szEndDebug);
            break;
        }
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected) {
            m_bml->SendIngameMessage("You've been disconnected from the server.");
        }
        m_bml->SendIngameMessage(s.c_str());
        cleanup();
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
            m_bml->SendIngameMessage("Connect failed. (ProblemDetectedLocally)");
            GetLogger()->Warn(pInfo->m_info.m_szEndDebug);
        }
        else {
            // NOTE: We could check the reason code for a normal disconnection
            m_bml->SendIngameMessage("Connect failed. (UnknownError)");
            GetLogger()->Warn("Unknown error. (%d->%d) %s", pInfo->m_eOldState, pInfo->m_info.m_eState, pInfo->m_info.m_szEndDebug);
        }
        m_bml->SendIngameMessage(s.c_str());
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
        m_bml->SendIngameMessage("Connected to server.");
        m_bml->SendIngameMessage("Logging in...");
        bmmo::login_request_v2_msg msg;
        db_.set_nickname(props_["playername"]->GetString());
        msg.nickname = props_["playername"]->GetString();
        msg.version = version;
        msg.cheated = m_bml->IsCheatEnabled();
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
    case bmmo::LoginAccepted: {
        /*status_->update("Connected");
        status_->paint(0xff00ff00);
        m_bml->SendIngameMessage("Logged in.");
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
        m_bml->SendIngameMessage("Logged in.");
        bmmo::login_accepted_v2_msg msg;
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        if (!msg.deserialize()) {
            GetLogger()->Error("Deserialize failed!");
        }
        GetLogger()->Info("Online players: ");
        
        for (auto& i : msg.online_players) {
            if (i.second.name == db_.get_nickname()) {
                db_.set_client_id(i.first);
            } else {
                db_.create(i.first, i.second.name, i.second.cheated);
            }
            GetLogger()->Info(i.second.name.c_str());
        }
        break;
    }
    case bmmo::PlayerConnected: {
        //bmmo::player_connected_msg msg;
        //msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        //msg.deserialize();
        //m_bml->SendIngameMessage((msg.name + " joined the game.").c_str());
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
        m_bml->SendIngameMessage(std::format("{} joined the game with cheat [{}].", msg.name, msg.cheated ? "on" : "off").c_str());
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
            m_bml->SendIngameMessage((state->name + " left the game.").c_str());
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
        m_bml->SendIngameMessage(print_msg.c_str());
        break;
    }
    case bmmo::LevelFinish: {
        auto* msg = reinterpret_cast<bmmo::level_finish_msg*>(network_msg->m_pData);

        // Prepare data...
        int score = msg->content.levelBouns + msg->content.points + msg->content.lifes * msg->content.lifeBouns;

        int total = int(msg->content.timeElapsed);
        int minutes = total / 60;
        int seconds = total % 60;
        int hours = minutes / 60;
        minutes = minutes % 60;
        int ms = int((msg->content.timeElapsed - total) * 1000);

        // Prepare message
        const std::string fmt_string = "{} has finished a map, scored {}, elapsed {:02d}:{:02d}:{:02d}.{:03d} in real time.";
        auto state = db_.get(msg->content.player_id);
        assert(state.has_value() || (db_.get_client_id() == msg->content.player_id));
        m_bml->SendIngameMessage(std::format(fmt_string,
            state.has_value() ? state->name : db_.get_nickname(), score, hours, minutes, seconds, ms).c_str());
        // TODO: Stop displaying objects on finish
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

        if (ocs->content.notify) {
            std::string s = std::format("{} turned cheat [{}].", state.has_value() ? state->name : db_.get_nickname(), ocs->content.state.cheated ? "on" : "off");
            m_bml->SendIngameMessage(s.c_str());
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
        m_bml->SendIngameMessage(str.c_str());
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
            m_bml->SendIngameMessage(str.c_str());
        }
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
        static_cast<BallanceMMOClient*>(this_instance_)->m_bml->SendIngameMessage("BallanceMMO has encountered a bug which is fatal. Please contact developer with this piece of log.");
        static_cast<BallanceMMOClient*>(this_instance_)->m_bml->SendIngameMessage("Nuking process in 5 seconds...");
        std::this_thread::sleep_for(std::chrono::seconds(5));

        std::terminate();
    }
}
