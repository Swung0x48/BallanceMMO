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
        init_template_balls();
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
        ping_ = std::make_shared<text_sprite>("T_MMO_PING", "Ping: --- ms", RIGHT_MOST, 0.0f);
        status_ = std::make_shared<text_sprite>("T_MMO_STATUS", "Disconnected", RIGHT_MOST, 0.025f);
        status_->paint(0xffff0000);

        m_bml->RegisterCommand(new CommandMMO([this](IBML* bml, const std::vector<std::string>& args) { OnCommand(bml, args); }));
        init_ = true;
    }
}

void BallanceMMOClient::OnProcess() {
    //poll_connection_state_changes();
    //poll_incoming_messages();

    poll_and_toggle_debug_info();

    if (!connected())
        return;

    std::unique_lock<std::mutex> bml_lk(bml_mtx_, std::try_to_lock);
    if (bml_lk) {
        int ping = get_status().m_nPing;
        ping_->update(std::format("Ping: {} ms", ping), false);

        if (m_bml->IsPlaying()) {
            auto ball = get_current_ball();
            if (player_ball_ == nullptr)
                player_ball_ = ball;

            check_on_trafo(ball);
            update_player_ball_state();
            assemble_and_send_state();

            process_username_label();
        }
    }
}

void BallanceMMOClient::OnStartLevel()
{
    if (!connected())
        return;

    player_ball_ = get_current_ball();
    ball_state_.type = ball_name_to_idx_[player_ball_->GetName()];
}

void BallanceMMOClient::OnExitGame()
{
    shutdown();
}

void BallanceMMOClient::OnUnload() {
    shutdown();
    client::destroy();
}

void BallanceMMOClient::OnCommand(IBML* bml, const std::vector<std::string>& args)
{
    auto help = [this](IBML* bml) {
        std::lock_guard<std::mutex> lk(bml_mtx_);
        bml->SendIngameMessage("BallanceMMO Help");
        bml->SendIngameMessage("/connect - Connect to server.");
        bml->SendIngameMessage("/disconnect - Disconnect from server.");
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
                } else if (connecting()) {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml->SendIngameMessage("Connecting in process, please wait...");
                }
                else {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    if (connect(props_["remote_addr"]->GetString())){
                        bml->SendIngameMessage("Connecting...");
                        network_thread_ = std::thread([this]() { run(); });
                    }
                    else
                        bml->SendIngameMessage("Connect to server failed.");
                }
            }
            else if (args[1] == "disconnect" || args[1] == "d") {
                if (!connecting() && !connected()) {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml->SendIngameMessage("Already disconnected.");
                }
                else {
                    //client_.disconnect();
                    //ping_->update("Ping: --- ms");
                    //status_->update("Disconnected");
                    //status_->paint(0xffff0000);
                    std::unique_lock<std::mutex> peer_lk(peer_mtx_);
                    peer_.clear();
                    shutdown();
                    if (network_thread_.joinable())
                        network_thread_.join();
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml->SendIngameMessage("Disconnected.");

                    ping_->update("Ping: --- ms");
                    status_->update("Disconnected");
                    status_->paint(0xffff0000);
                }
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
}

void BallanceMMOClient::OnPeerTrafo(uint64_t id, int from, int to)
{
    GetLogger()->Info("OnPeerTrafo, %d -> %d", from, to);
    PeerState& peer = peer_[id];
    peer.current_ball = to;
    peer.balls[from]->Show(CKHIDE);
    peer.balls[to]->Show(CKSHOW);
}

void BallanceMMOClient::on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
    GetLogger()->Info("Connection status changed. %d -> %d", pInfo->m_eOldState, pInfo->m_info.m_eState);
    switch (pInfo->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_None:
        ping_->update("Ping: --- ms");
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
        break;
    }
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
        ping_->update("Ping: --- ms");
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

        interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
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
        msg.nickname = props_["playername"]->GetString();
        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
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
        GetLogger()->Info("%ld: (%.2lf, %.2lf, %.2lf), (%.2lf, %.2lf, %.2lf, %.2lf)",
            obs->content.player_id,
            obs->content.state.position.x,
            obs->content.state.position.y,
            obs->content.state.position.z,
            obs->content.state.rotation.x,
            obs->content.state.rotation.y,
            obs->content.state.rotation.z,
            obs->content.state.rotation.w);
        break;
    }
    case bmmo::LoginAccepted: {
        status_->update("Connected");
        status_->paint(0xff00ff00);
        m_bml->SendIngameMessage("Logged in.");
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
        static_cast<BallanceMMOClient*>(this_instance_)->m_bml->SendIngameMessage("BallanceMMO has encountered a bug. Please contact developer with this piece of log.");
        static_cast<BallanceMMOClient*>(this_instance_)->GetLogger()->Error("We've encountered a bug. Please contact developer with this piece of log.");
        static_cast<BallanceMMOClient*>(this_instance_)->GetLogger()->Error("Nuking process...");
        std::this_thread::sleep_for(std::chrono::seconds(5));

        std::terminate();
    }
}
