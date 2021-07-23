#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
	return new BallanceMMOClient(bml);
}

void BallanceMMOClient::OnLoad()
{
	GetConfig()->SetCategoryComment("Remote", "Which server to connect to?");
	IProperty* tmp_prop = GetConfig()->GetProperty("Remote", "ServerAddress");
	tmp_prop->SetComment("Remote server address, it could be an IP address or a domain name.");
	tmp_prop->SetDefaultString("127.0.0.1");
	props_["remote_addr"] = tmp_prop;
	tmp_prop = GetConfig()->GetProperty("Remote", "Port");
	tmp_prop->SetComment("The port that server is running on.");
	tmp_prop->SetDefaultInteger(50000);
	props_["remote_port"] = tmp_prop;

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

void BallanceMMOClient::OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj)
{
    if (strcmp(filename, "3D Entities\\Balls.nmo") == 0) {
        CKDataArray* physicalized_ball = m_bml->GetArrayByName("Physicalize_GameBall");
        for (int i = 0; i < physicalized_ball->GetRowCount(); i++) {
            CK3dObject* ball;
            std::string ball_name;
            ball_name.resize(physicalized_ball->GetElementStringValue(i, 0, nullptr), '\0');
            physicalized_ball->GetElementStringValue(i, 0, &ball_name[0]);
            ball_name.pop_back();
            ball = m_bml->Get3dObjectByName(ball_name.c_str());

            CKDependencies dep;
            dep.Resize(40); dep.Fill(0);
            dep.m_Flags = CK_DEPENDENCIES_CUSTOM;
            dep[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
            dep[CKCID_MESH] = CK_DEPENDENCIES_COPY_MESH_MATERIAL;
            dep[CKCID_3DENTITY] = CK_DEPENDENCIES_COPY_3DENTITY_MESH;
            ball = static_cast<CK3dObject*>(m_bml->GetCKContext()->CopyObject(ball, &dep, "_Peer_"));
            for (int j = 0; j < ball->GetMeshCount(); j++) {
                CKMesh* mesh = ball->GetMesh(j);
                for (int k = 0; k < mesh->GetMaterialCount(); k++) {
                    CKMaterial* mat = mesh->GetMaterial(k);
                    mat->EnableAlphaBlend();
                    mat->SetSourceBlend(VXBLEND_SRCALPHA);
                    mat->SetDestBlend(VXBLEND_INVSRCALPHA);
                    VxColor color = mat->GetDiffuse();
                    color.a = 0.5f;
                    mat->SetDiffuse(color);
                    m_bml->SetIC(mat);
                }
            }
            template_balls_[i] = ball;
            ball_name_to_idx_[ball_name] = i; // "Ball_Xxx"
        }
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

        m_bml->RegisterCommand(new CommandMMO(client_, props_, bml_mtx_, ping_, status_));
        init_ = true;
    }
}

void BallanceMMOClient::OnProcess() {
    if (m_bml->GetInputManager()->IsKeyPressed(CKKEY_BACKSLASH)) {
        ping_->toggle();
        status_->toggle();
    }

    if (!client_.connected())
        return;

    std::unique_lock<std::mutex> bml_lk(bml_mtx_, std::try_to_lock);
    if (bml_lk && m_bml->IsPlaying()) {
        auto ball = get_current_ball();
        if (strcmp(ball->GetName(), player_ball_->GetName()) != 0) {
            // OnTrafo
            GetLogger()->Info("OnTrafo, %s -> %s", player_ball_->GetName(), ball->GetName());
            // Update current player ball
            player_ball_ = ball;
            ball_state_.type = ball_name_to_idx_[player_ball_->GetName()];
        }
        player_ball_->GetPosition(&ball_state_.position);
        player_ball_->GetQuaternion(&ball_state_.rotation);
        ammo::common::message<PacketType> msg;
        msg.header.id = PacketType::GameState;
        msg << ball_state_;
        std::unique_lock client_lk(client_mtx_, std::try_to_lock);
        if (client_lk)
            client_.send(msg);
    }
}

void BallanceMMOClient::OnStartLevel()
{
    if (!client_.connected())
        return;

    player_ball_ = get_current_ball();
    ball_state_.type = ball_name_to_idx_[player_ball_->GetName()];
}

void BallanceMMOClient::OnExitGame()
{
	try {
		client_.disconnect();
		client_.shutdown();
	} catch (std::exception& e) {
		GetLogger()->Error(e.what());
	}
}

void BallanceMMOClient::OnUnload() {
	
}

void BallanceMMOClient::OnMessage(ammo::common::owned_message<PacketType>& msg)
{
    if (!client_.connected()) {
        if (msg.message.header.id == ConnectionAccepted) {
            std::unique_lock lk(bml_mtx_);
            client_.confirm_validation();
            status_->update("Connected");
            status_->paint(0xff00ff00);
            m_bml->SendIngameMessage("Accepted by server!");
            m_bml->SendIngameMessage((std::string("Welcome back, ") + props_["playername"]->GetString()).c_str());

            m_bml->AddTimerLoop(1000.0f, [this]() {
                ammo::common::message<PacketType> msg;
                uint64_t now = std::chrono::system_clock::now().time_since_epoch().count();
                msg << now;
                msg.header.id = Ping;
                std::unique_lock client_lk(client_mtx_, std::try_to_lock);

                if (client_lk) {
                    client_.send(msg);
                }
                return client_.connected();
            });
        }
        else if (msg.message.header.id == ConnectionChallenge) {
            uint64_t checksum;
            msg.message >> checksum;
            checksum = encode_for_validation(checksum);
            msg.message.clear();
            msg.message << checksum;
            ammo::entity::string<PacketType> str = props_["playername"]->GetString();
            str.serialize(msg.message);
            msg.message.header.id = ConnectionResponse;
            client_.send(msg.message);
        }
        else if (msg.message.header.id == Denied) {
            std::unique_lock lk(bml_mtx_);
            status_->update("Disconnected (Rejected)");
            status_->paint(0xffff0000);
            m_bml->SendIngameMessage("Rejected by server.");
        }
    }
    else {
        switch (msg.message.header.id) {
            case PacketFragment: {
                break;
            }
            case Denied: {
                break;
            }
            case Ping: {
                std::scoped_lock<std::mutex> bml_lk_(bml_mtx_);
                auto now = std::chrono::system_clock::now().time_since_epoch().count();
                uint64_t then; msg.message >> then;
                auto ping = now - then; // in microseconds
                ping_->update(std::format("Ping: {:3} ms", ping / 1000), false);
                break;
            }
            case GameState: {
                std::unique_lock lk(bml_mtx_);
                break;
            }
            default: {
                std::scoped_lock<std::mutex> bml_lk_(bml_mtx_);
                GetLogger()->Warn("Unknown message ID: %d", msg.message.header.id);
            }
        }
    }
}
