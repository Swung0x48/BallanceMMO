#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
	return new BallanceMMOClient(bml);
}

std::string hash_sha256(std::ifstream& fs)
{
	const size_t BUF_SIZE = 256;
	SHA256 sha256;
	std::vector<char> buffer(BUF_SIZE, 0);
	while (!fs.eof())
	{
		fs.read(buffer.data(), buffer.size());
		std::streamsize read_size = fs.gcount();
		sha256.add(buffer.data(), read_size);
	}
	return sha256.getHash();
}

std::string normalize(const std::string& input) {
	if (input.length() <= 5)
		return "Ball_Err";

	if (input.substr(0, 5) != "Ball_")
		return "Ball_Err";

	size_t i = 6;
	for (; i < input.length(); i++) {
		if (input[i] == '_')
			break;
	}

	return input.substr(0, i);
}

void BallanceMMOClient::OnLoad()
{
	GetConfig()->SetCategoryComment("Remote", "Which server to connect to?");
	IProperty* tmp_prop = GetConfig()->GetProperty("Remote", "ServerAddress");
	tmp_prop->SetComment("Remote server address, it could be an IP address or a domain name.");
	tmp_prop->SetDefaultString("139.224.23.40");
	props_["remote_addr"] = tmp_prop;
	tmp_prop = GetConfig()->GetProperty("Remote", "Port");
	tmp_prop->SetComment("The port that server is running on.");
	tmp_prop->SetDefaultInteger(60000);
	props_["remote_port"] = tmp_prop;
	client_.connect(props_["remote_addr"]->GetString(), props_["remote_port"]->GetInteger());

	GetConfig()->SetCategoryComment("Player", "Who are you?");
	tmp_prop = GetConfig()->GetProperty("Player", "Playername");
	tmp_prop->SetComment("Your name please?");
	std::srand(std::time(nullptr));
	int random_variable = std::rand() % 1000;
	std::stringstream ss;
	ss << "Player" << std::setw(3) << std::setfill('0') << random_variable;
	tmp_prop->SetDefaultString(ss.str().c_str());
	props_["playername"] = tmp_prop;

	//gui_ = std::make_unique<BGui::Gui>();
	ping_text_ = std::make_unique<BGui::Label>("M_MMO_Ping");
	//ping_text_ = gui_->AddTextLabel("M_MMO_Ping", "Ping: --- ms", ExecuteBB::GAMEFONT_01, 0, 0, 0.99f, 0.03f);
	ping_text_->SetText("Ping: --- ms");
	ping_text_->SetFont(ExecuteBB::GAMEFONT_01);
	ping_text_->SetPosition(Vx2DVector(0, 0));
	ping_text_->SetSize(Vx2DVector(0.99f, 0.03f));
	ping_text_->SetAlignment(ALIGN_RIGHT);
	ping_text_->SetVisible(false);
	auto lk = std::unique_lock<std::mutex>(ping_char_mtx_, std::try_to_lock);
	if (lk)
		strcpy(ping_char_, "Ping: --- ms");
	gui_avail_ = true;
}

void BallanceMMOClient::OnPreStartMenu() {
	if (client_.is_connected()) {
		connected_ = true;
		while (!client_.get_incoming_messages().empty()) {
			auto msg = client_.get_incoming_messages().pop_front().msg;
			process_incoming_message(msg);
		}
	}

	if (gui_avail_)
		ping_text_->SetVisible(false);

	pinging_.stop();
}

void BallanceMMOClient::OnPostStartMenu() {
	if (client_.is_connected()) {
		connected_ = true;
		while (!client_.get_incoming_messages().empty()) {
			auto msg = client_.get_incoming_messages().pop_front().msg;
			process_incoming_message(msg);
		}
	}

	if (!receiving_msg_) {
		receiving_msg_ = true;
		msg_receive_thread_ = std::thread([this]() {
			std::unique_lock<std::mutex> lk(start_receiving_mtx);
			//while (!ready_to_rx_)
				//start_receiving_cv_.wait(lk);

			while (receiving_msg_)
			{
				while (client_.get_incoming_messages().empty())
					client_.get_incoming_messages().wait();

				auto msg = client_.get_incoming_messages().pop_front();
				process_incoming_message(msg.msg);
			}
		});
		//msg_receive_thread_.detach();
	}
}

void BallanceMMOClient::OnProcess()
{
	if (m_bml->IsPlaying()) {
		//loop_count_++;

		if (!client_.is_connected() && connected_) {
			auto lk = std::unique_lock<std::mutex>(ping_char_mtx_, std::try_to_lock);
			if (lk)
				strcpy(ping_char_, "Ping: --- ms");
			m_bml->SendIngameMessage("You've been disconnected from server.");
			client_.disconnect();
			connected_ = false;
			//client_.connect(props_["remote_addr"]->GetString(), props_["remote_port"]->GetInteger());
		}

		auto ball = get_current_ball();
		if (strcmp(ball->GetName(), player_ball_->GetName()) != 0) {
			// OnTrafo
			GetLogger()->Info("OnTrafo, %s -> %s", player_ball_->GetName(), ball->GetName());
			
			VxVector vec;
			ball->GetPosition(&vec);
			
			// Update current player ball
			player_ball_ = ball;
			ball_state_.type = ball_name_to_idx_[player_ball_->GetName()];
		}

		player_ball_->GetPosition(&ball_state_.position);
		player_ball_->GetQuaternion(&ball_state_.rotation);
		
		msg_.clear();
		msg_.header.id = MsgType::BallState;
		msg_ << ball_state_;
		client_.broadcast_message(msg_);

		if (gui_avail_) {
			auto lk = std::unique_lock<std::mutex>(ping_char_mtx_, std::try_to_lock);
			if (lk) {
				ping_text_->SetText(ping_char_);
				ping_text_->SetVisible(true);
			}
		}
		if (ping_text_)
			ping_text_->Process();

		if (m_bml->GetInputManager()->IsKeyPressed(CKKEY_TAB)) {
			show_player_name_ = !show_player_name_;
		}

		if (show_player_name_) {
#ifdef DEBUG
			GetLogger()->Info("peer ball size: %d", peer_balls_.size());
#endif // DEBUG

			for (auto& i : peer_balls_) {
				if (i.second.balls[i.second.current_ball] == nullptr)
					continue;

				if (i.second.username_label == nullptr) {
					i.second.username_label = std::make_unique<BGui::Label>(("MMO_Name_" + std::to_string(i.first)).c_str());
					i.second.username_label->SetText(i.second.player_name.c_str());
					i.second.username_label->SetFont(ExecuteBB::GAMEFONT_03);
					i.second.username_label->SetPosition(Vx2DVector(0, 0));
					i.second.username_label->SetSize(Vx2DVector(0.1f, 0.03f));

					//i.second.username_label = gui_->AddTextLabel(("MMO_Name_" + std::to_string(i.first)).c_str(), i.second.player_name.c_str(), ExecuteBB::GAMEFONT_03, 0, 0, 0.1f, 0.03f);
					i.second.username_label->SetAlignment(ALIGN_CENTER);
					i.second.username_label->SetVisible(true);
				}

				VxRect viewport;
				m_bml->GetRenderContext()->GetViewRect(viewport);
				VxRect rect;
				i.second.balls[i.second.current_ball]->GetRenderExtents(rect);
				Vx2DVector pos((rect.left + rect.right) / 2.0f / viewport.right, rect.top / viewport.bottom);
#ifdef _DEBUG
				GetLogger()->Info("pos: %.0f, %.0f", (rect.left + rect.right) / 2.0f, (rect.top + rect.bottom) / 2.0f);
				GetLogger()->Info("vec: %.2f, %.2f", pos.x, pos.y);
#endif // _DEBUG
				//i.second.username_label->SetText(i.second.player_name.c_str());
				//GetLogger()->Info(i.second.player_name.c_str());
				i.second.username_label->SetVisible(true);
				i.second.username_label->SetPosition(pos);
				i.second.username_label->Process();
			}
		}
	}
}

void BallanceMMOClient::OnStartLevel()
{
	player_ball_ = get_current_ball();
	ball_state_.type = ball_name_to_idx_[player_ball_->GetName()];

	ready_to_rx_ = true;
	start_receiving_cv_.notify_one();

	/*if (!receiving_msg_) {
		receiving_msg_ = true;
		msg_receive_thread_ = std::thread([this]() {
			std::unique_lock<std::mutex> lk(start_receiving_mtx);
			while (!ready_to_rx_)
				start_receiving_cv_.wait(lk);

			while (receiving_msg_)
			{
				while (client_.get_incoming_messages().empty())
					client_.get_incoming_messages().wait();

				auto msg = client_.get_incoming_messages().pop_front();
				process_incoming_message(msg.msg);
				
				if (client_.get_incoming_messages().size() > MSG_MAX_SIZE)
					client_.get_incoming_messages().clear();
			}
		});
		msg_receive_thread_.detach();
	}*/

	pinging_.setInterval([&]() {
		GetLogger()->Info("Pinging Server...");
		client_.ping_server();
	}, PING_INTERVAL);
}

void BallanceMMOClient::OnBallNavActive() {
	//ready_to_rx_ = true;
	//start_receiving_cv_.notify_one();

	/*send_ball_state_.setInterval([&]() {
		msg_.clear();
		msg_.header.id = MsgType::BallState;
		msg_ << ball_state_;
		client_.broadcast_message(msg_);
	}, SEND_BALL_STATE_INTERVAL);*/
}

void BallanceMMOClient::OnBallNavInactive() {
	//send_ball_state_.stop();
}

void BallanceMMOClient::OnPreEndLevel()
{
	blcl::net::message<MsgType> msg;
	msg.header.id = MsgType::FinishLevel;
	client_.send(msg);
}

void BallanceMMOClient::OnPreExitLevel()
{
	blcl::net::message<MsgType> msg;
	msg.header.id = MsgType::ExitMap;
	client_.send(msg);
}

void BallanceMMOClient::OnUnload()
{
	receiving_msg_ = false;
	//send_ball_state_.stop();
	pinging_.stop();
	client_.get_incoming_messages().clear();
	if (msg_receive_thread_.joinable())
		msg_receive_thread_.join();

	//delete ping_text_;
	//delete gui_;
	client_.disconnect();
}

void BallanceMMOClient::OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj)
{
	//ping_text_->SetVisible(true);

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

	if (isMap) {
		std::string filename_string(filename);
		std::filesystem::path path = std::filesystem::current_path().parent_path().append(filename_string[0] == '.' ? filename_string.substr(3, filename_string.length()) : filename_string);
		std::ifstream map(path, std::ios::in | std::ios::binary);
		map_hash_ = hash_sha256(map);
		m_bml->SendIngameMessage(map_hash_.c_str());
		blcl::net::message<MsgType> msg;
		msg.header.id = MsgType::EnterMap;
		client_.send(msg);
	}
}

void BallanceMMOClient::process_incoming_message(blcl::net::message<MsgType>& msg)
{
	if (msg.size() > MSG_MAX_SIZE) {
		GetLogger()->Error("A message exceeds MAX_MSG_SIZE (%d > %u).", msg.size(), MAX_MSG_SIZE);
		GetLogger()->Error("Message identifier: ", msg.header.id);
		GetLogger()->Error("It's likely a junk message and will be discarded");
		GetLogger()->Warn("If you ever seen this, it's likely something really nasty happened.");
		GetLogger()->Warn("Please report this incident to the developer.");
		return;
	}

	switch (msg.header.id) {
		case MsgType::ServerAccept: {
			m_bml->SendIngameMessage("Connection established.");
			break;
		}
		case MsgType::ClientValidated: {
			uint64_t client_id; msg >> client_id;
			GetLogger()->Info("Assigned client ID: %I64d", client_id);
			break;
		}
		case MsgType::UsernameReq: {
			GetLogger()->Info("On UsernameReq");
			msg >> client_.max_username_length_;
			client_.send_username(std::string(props_["playername"]->GetString()));
			break;
		}
		case MsgType::Username: {
			uint64_t client_id; msg >> client_id;
			std::string name(reinterpret_cast<const char*>(msg.body.data()));
			add_active_client(client_id, name);
			GetLogger()->Info("Update client %I64d name to %s", client_id, name.c_str());
			break;
		}
		case MsgType::UsernameAck: {
			m_bml->SendIngameMessage(("Welcome back, " + std::string(reinterpret_cast<const char*>(msg.body.data()))).c_str());
			break;
		}
		case MsgType::ClientDisconnect: {
			uint64_t client_id; msg >> client_id;
			std::stringstream ss;
			ss << reinterpret_cast<const char*>(msg.body.data()) << " left the game.";
			m_bml->SendIngameMessage(ss.str().c_str());
			if (!hide_player_ball(client_id)) {
				GetLogger()->Warn("Hide %I64u failed.", client_id);
			}
			break;
		}
		case MsgType::ServerPing: {
			GetLogger()->Info("On ServerPing");
			std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
			std::chrono::system_clock::time_point sent;
			msg >> sent;
			unsigned long long ping = std::chrono::duration_cast<std::chrono::milliseconds>(now - sent).count();
			GetLogger()->Info("%I64d ms", ping);
			auto lk = std::unique_lock<std::mutex>(ping_char_mtx_);
			sprintf(ping_char_, "Ping: %02lld ms", ping);
			
			//if (ping > PING_TIMEOUT)
				//client_.get_incoming_messages().clear();

			break;
		}
		case MsgType::MapHashReq: {
			msg.clear();
			msg.header.id = MsgType::MapHash;
			msg.write(map_hash_.c_str(), map_hash_.length() + 1);
			client_.send(msg);
			break;
		}
		case MsgType::BallState: {
			GetLogger()->Info("On BallState");

			uint64_t remote_id;
			msg >> remote_id;
			BallState msg_state;
			msg >> msg_state;
//#ifdef DEBUG
			GetLogger()->Info("%I64d, B: %d, (%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f, %.2f)",
				remote_id,
				msg_state.type,
				msg_state.position.x,
				msg_state.position.y,
				msg_state.position.z,
				msg_state.rotation.x,
				msg_state.rotation.y,
				msg_state.rotation.z,
				msg_state.rotation.w);
//#endif // DEBUG
			if (peer_balls_.find(remote_id) == peer_balls_.end() || peer_balls_[remote_id].balls[0] == nullptr) {
				// If message comes from a new client, then init balls and set IC
				PeerState& state = peer_balls_[remote_id];

				//PeerState state;
				for (size_t i = 0; i < ball_name_to_idx_.size(); i++)
					state.balls[i] = init_spirit_ball(i, remote_id);
				state.current_ball = msg_state.type;
				//peer_balls_[remote_id] = std::move(state);
				peer_balls_[remote_id].balls[msg_state.type]->Show(CKSHOW);
			}

			if (peer_balls_.size() == 0)
				return;
			auto current_ball = peer_balls_[remote_id].current_ball;
			auto new_ball = msg_state.type;
			if (current_ball != new_ball) {
				peer_balls_[remote_id].balls[current_ball]->Show(CKHIDE);
				//peer_balls_[remote_id].balls[new_ball]->Show(CKSHOW);

				peer_balls_[remote_id].current_ball = new_ball;
			}
			peer_balls_[remote_id].balls[new_ball]->Show(CKSHOW);
			peer_balls_[remote_id].balls[new_ball]->SetPosition(msg_state.position);
			peer_balls_[remote_id].balls[new_ball]->SetQuaternion(msg_state.rotation);
			break;
		}
		case MsgType::MapHashAck: {
			for (auto& ball: peer_balls_)
				hide_player_ball(ball.first);
			break;
		}
		case MsgType::ExitMap: {
			uint64_t client_id; msg >> client_id;
			hide_player_ball(client_id);
			m_bml->SendIngameMessage((peer_balls_[client_id].player_name + " left this map.").c_str());
			peer_balls_.unsafe_erase(client_id);
			//delete peer_balls_.at(client_id).username_label;
			break;
		}
		case MsgType::FinishLevel: {
			uint64_t client_id; msg >> client_id;
			m_bml->SendIngameMessage((peer_balls_[client_id].player_name + " finished this level.").c_str());
			break;
		}
		case MsgType::PlayerJoined: {
			m_bml->SendIngameMessage((std::string(reinterpret_cast<const char*>(msg.body.data())) + " joined the game.").c_str());
			break;
		}
		default: {
			GetLogger()->Warn("Unknown message ID: %u", msg.header.id);
			GetLogger()->Warn("Message size: %u", msg.header.size);
			GetLogger()->Warn("If you ever seen this, it's likely something really nasty happened.");
			GetLogger()->Warn("Please report this incident to the developer.");

			break;
		}
	}
}

CK3dObject* BallanceMMOClient::init_spirit_ball(int ball_index, uint64_t id) {
	CKDependencies dep;
	dep.Resize(40); dep.Fill(0);
	dep.m_Flags = CK_DEPENDENCIES_CUSTOM;
	dep[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
	dep[CKCID_MESH] = CK_DEPENDENCIES_COPY_MESH_MATERIAL;
	dep[CKCID_3DENTITY] = CK_DEPENDENCIES_COPY_3DENTITY_MESH;
	CK3dObject* ball = static_cast<CK3dObject*>(m_bml->GetCKContext()->CopyObject(template_balls_[ball_index], &dep, std::to_string(id).c_str()));
	for (int j = 0; j < ball->GetMeshCount(); j++) {
		CKMesh* mesh = ball->GetMesh(j);
		for (int k = 0; k < mesh->GetMaterialCount(); k++) {
			CKMaterial* mat = mesh->GetMaterial(k);
			m_bml->SetIC(mat);
		}
	}
	return ball;
}