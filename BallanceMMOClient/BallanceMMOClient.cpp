#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
	return new BallanceMMOClient(bml);
}

void BallanceMMOClient::OnLoad()
{
    init_config();
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
    poll_and_toggle_debug_info();

    /*if (!client_.connected())
        return;*/

    std::unique_lock<std::mutex> bml_lk(bml_mtx_, std::try_to_lock);
    if (bml_lk && m_bml->IsPlaying()) {
        auto ball = get_current_ball();
        if (player_ball_ == nullptr)
            player_ball_ = ball;

        check_on_trafo(ball);
        update_player_ball_state();
        assemble_and_send_state();
        /*ammo::common::message<PacketType> msg;
        msg.header.id = PacketType::GameState;
        msg << id_;
        msg << ball_state_;
        std::unique_lock client_lk(client_mtx_, std::try_to_lock);
        if (client_lk)
            client_.send(msg);*/

        process_username_label();
    }
}

void BallanceMMOClient::OnStartLevel()
{
    /*if (!client_.connected())
        return;*/

    player_ball_ = get_current_ball();
    ball_state_.type = ball_name_to_idx_[player_ball_->GetName()];
}

void BallanceMMOClient::OnExitGame()
{
	
}

void BallanceMMOClient::OnUnload() {

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
            /*if (args[1] == "connect" || args[1] == "c") {
                if (client_.connected()) {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml->SendIngameMessage("Already connected.");
                }
                else if (client_.get_state() == ammo::role::client_state::Disconnected) {
                    status_->update("Pending");
                    status_->paint(0xFFF6A71B);
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    if (client_.connect(props_["remote_addr"]->GetString(), props_["remote_port"]->GetInteger()))
                        bml->SendIngameMessage("Connection request sent to server. Waiting for reply...");
                    else
                        bml->SendIngameMessage("Connect to server failed.");
                }
            }
            else if (args[1] == "disconnect" || args[1] == "d") {
                if (client_.get_state() == ammo::role::client_state::Disconnected) {
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml->SendIngameMessage("Already disconnected.");
                }
                else {
                    client_.disconnect();
                    ping_->update("Ping: --- ms");
                    status_->update("Disconnected");
                    status_->paint(0xffff0000);
                    std::unique_lock<std::mutex> peer_lk(peer_mtx_);
                    peer_.clear();
                    std::lock_guard<std::mutex> lk(bml_mtx_);
                    bml->SendIngameMessage("Disconnected.");
                }
            }*/
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
