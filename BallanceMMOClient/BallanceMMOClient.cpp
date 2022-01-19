#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
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
        objects_.init_players();
    }

    if (strcmp(filename, "3D Entities\\Gameplay.nmo") == 0) {
        current_level_array_ = m_bml->GetArrayByName("CurrentLevel");
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

    poll_status_toggle();
    poll_local_input();

    if (!connected())
        return;

    std::unique_lock<std::mutex> bml_lk(bml_mtx_, std::try_to_lock);

    if (bml_lk) {
        if (m_bml->IsPlaying()) {
            auto ball = get_current_ball();
            if (player_ball_ == nullptr)
                player_ball_ = ball;

            check_on_trafo(ball);
            poll_player_ball_state();

            asio::post(thread_pool_, [this]() {
                assemble_and_send_state();
            });

            objects_.update();
        }
    }
}

void BallanceMMOClient::OnStartLevel()
{
    /*if (!connected())
        return;*/

    player_ball_ = get_current_ball();
    local_ball_state_.type = db_.get_ball_id(player_ball_->GetName());

    //objects_.destroy_all_objects();
    //objects_.init_players();
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
                bool is_first = true;
                db_.for_each([&ss, &is_first](const std::pair<const HSteamNetConnection, PlayerState>& pair) {
                    if (is_first) {
                        ss << pair.second.name;
                        is_first = false;
                    }
                    else {
                        ss << ", " << pair.second.name;
                    }
                    return true;
                    });

                m_bml->SendIngameMessage(ss.str().c_str());
            } 
            //else if (args[1] == "p") {
            //    objects_.physicalize_all();
            //} else if (args[1] == "f") {
            //    //bbSetForce = ExecuteBB::CreateSetPhysicsForce();
            //    //SetForce(bbSetForce, player_ball_, VxVector(0, 0, 0), player_ball_, VxVector(1, 0, 0), m_bml->Get3dObjectByName("Cam_OrientRef"), .43f);
            //    ExecuteBB::SetPhysicsForce(player_ball_, VxVector(0, 0, 0), player_ball_, VxVector(1, 0, 0), m_bml->Get3dObjectByName("Cam_OrientRef"), .43f);
            //} else if (args[1] == "u") {
            //    ExecuteBB::UnsetPhysicsForce(player_ball_);
            //    //UnsetPhysicsForce(bbSetForce, player_ball_);
            //}
            break;
        }
        case 3: {
            if (args[1] == "s" || args[1] == "say") {
                bmmo::chat_msg msg{};
                msg.chat_content = args[2];
                msg.serialize();

                send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
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
        if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
            // Note: we could distinguish between a timeout, a rejected connection,
            // or some other transport problem.
            m_bml->SendIngameMessage("Connect failed. (ClosedByPeer)");
            GetLogger()->Warn(pInfo->m_info.m_szEndDebug);
            break;
        }
        cleanup();
        break;
    }
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
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
        bmmo::login_request_msg msg;
        db_.set_nickname(props_["playername"]->GetString());
        msg.nickname = props_["playername"]->GetString();
        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        ping_thread_ = std::thread([this]() {
            while (connected()) {
                auto status = get_status();
                std::string str = pretty_status(status);
                ping_->update(str, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
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
        assert(success);
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
        status_->update("Connected");
        status_->paint(0xff00ff00);
        m_bml->SendIngameMessage("Logged in.");
        bmmo::login_accepted_msg msg;
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        GetLogger()->Info("Online players: ");
        for (auto& i : msg.online_players) {
            if (i.second == db_.get_nickname()) {
                db_.set_client_id(i.first);
            }
            db_.create(i.first, i.second);
        }
        break;
    }
    case bmmo::PlayerConnected: {
        bmmo::player_connected_msg msg;
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();
        m_bml->SendIngameMessage((msg.name + " joined the game.").c_str());
        GetLogger()->Info("Creating state entry for %u, %s", msg.connection_id, msg.name.c_str());
        db_.create(msg.connection_id, msg.name);

        // TODO: call this when the player enters a map
        if (m_bml->IsIngame())
            objects_.init_player(msg.connection_id, msg.name);
        break;
    }
    case bmmo::PlayerDisconnected: {
        bmmo::player_disconnected_msg* msg = reinterpret_cast<bmmo::player_disconnected_msg *>(network_msg->m_pData);
        auto state = db_.get(msg->content.connection_id);
        assert(state.has_value());
        m_bml->SendIngameMessage((state->name + " left the game.").c_str());
        db_.remove(msg->content.connection_id);
        objects_.remove(msg->content.connection_id);
        break;
    }
    case bmmo::Chat: {
        bmmo::chat_msg msg{};
        msg.raw.write(reinterpret_cast<char*>(network_msg->m_pData), network_msg->m_cbSize);
        msg.deserialize();

        std::string print_msg;

        if (msg.player_id == k_HSteamNetConnection_Invalid)
            print_msg = std::format("(Server): {}", msg.chat_content);
        else {
            auto state = db_.get(msg.player_id);
            assert(state.has_value());
            print_msg = std::format("[{}]: {}", state->name, msg.chat_content);
        }
        m_bml->SendIngameMessage(print_msg.c_str());
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
