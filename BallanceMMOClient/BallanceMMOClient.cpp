#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
	return new BallanceMMOClient(bml);
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
	IProperty* tmp_prop = GetConfig()->GetProperty("Remote", "Server Address");
	tmp_prop->SetComment("Remote server address, it could be an IP address or a domain name.");
	tmp_prop->SetDefaultString("139.224.23.40");
	props_["remote_addr"] = tmp_prop;
	tmp_prop = GetConfig()->GetProperty("Remote", "Port");
	tmp_prop->SetComment("The port that server is running on.");
	tmp_prop->SetDefaultInteger(60000);
	props_["remote_port"] = tmp_prop;
	client_.connect(props_["remote_addr"]->GetString(), props_["remote_port"]->GetInteger());

	gui_ = new BGui::Gui;
	ping_text_ = gui_->AddTextLabel("M_MMO_Ping", "Ping: --- ms", ExecuteBB::GAMEFONT_01, 0, 0, 0.99f, 0.03f);
	ping_text_->SetAlignment(ALIGN_RIGHT);
	ping_text_->SetVisible(false);
	gui_avail_ = true;
}

void BallanceMMOClient::OnPostStartMenu() {
	if (client_.is_connected()) {
		m_bml->SendIngameMessage("Connected!");
		client_.get_incoming_messages().clear();
	}

	if (gui_avail_)
		ping_text_->SetVisible(false);
}

void BallanceMMOClient::OnProcess()
{
	if (m_bml->IsPlaying()) {
		loop_count_++;

		if (!client_.is_connected()) {
			auto lk = std::scoped_lock<std::mutex>(ping_char_mtx_);
			strcpy(ping_char_, "Ping: --- ms");
			//client_.connect(props_["remote_addr"]->GetString(), props_["remote_port"]->GetInteger());
		}

		auto ball = get_current_ball();
		if (strcmp(ball->GetName(), player_ball_->GetName()) != 0) {
			// OnTrafo
			GetLogger()->Info("OnTrafo, %s -> %s", player_ball_->GetName(), ball->GetName());
			//spirit_ball_ = template_balls_[ball_name_to_idx_[ball->GetName()[5]]];
			
			VxVector vec;
			ball->GetPosition(&vec);
			//spirit_ball_->SetPosition(vec);
			//spirit_ball_->Show(CKSHOW);
			
			// Update current player ball
			player_ball_ = ball;
			ball_state_.type = ball_name_to_idx_[player_ball_->GetName()];
		}

		player_ball_->GetPosition(&ball_state_.position);
		player_ball_->GetQuaternion(&ball_state_.rotation);
		

		//if (m_bml->IsIngame() && !client_.get_incoming_messages().empty()) {
		//	auto msg = client_.get_incoming_messages().pop_front();
		//	process_incoming_message(msg.msg);
		//}
		if (gui_avail_) {
			auto lk = std::scoped_lock<std::mutex>(ping_char_mtx_);
			ping_text_->SetText(ping_char_);
			ping_text_->SetVisible(true);
		}
		if (ping_text_)
			ping_text_->Process();
	}
}

void BallanceMMOClient::OnStartLevel()
{
	player_ball_ = get_current_ball();
	ball_state_.type = ball_name_to_idx_[player_ball_->GetName()];

	if (!receiving_msg_) {
		receiving_msg_ = true;
		msg_receive_thread_ = std::thread([this]() {
			std::unique_lock<std::mutex> lk(start_receiving_mtx);
			while (!ready_to_rx_)
				start_receiving_cv_.wait(lk);

			while (receiving_msg_)
			{
				while (client_.get_incoming_messages().empty())
					client_.get_incoming_messages().wait();

				//GetLogger()->Info("%d msg", client_.get_incoming_messages().size());
				auto msg = client_.get_incoming_messages().pop_front();
				process_incoming_message(msg.msg);
				
				if (client_.get_incoming_messages().size() > MSG_MAX_SIZE)
					client_.get_incoming_messages().clear();
			}
		});
		msg_receive_thread_.detach();
	}

	pinging_.setInterval([&]() {
		GetLogger()->Info("Pinging Server...");
		client_.ping_server();
	}, 1000);
}

void BallanceMMOClient::OnBallNavActive() {
	ready_to_rx_ = true;
	start_receiving_cv_.notify_one();

	send_ball_state_.setInterval([&]() {
		msg_.clear();
		msg_.header.id = MsgType::MessageAll;
		msg_ << ball_state_;
		client_.broadcast_message(msg_);
	}, 15);
}

void BallanceMMOClient::OnBallNavInactive() {
	send_ball_state_.stop();
}

void BallanceMMOClient::OnUnload()
{
	receiving_msg_ = false;
	client_.get_incoming_messages().clear();
	if (msg_receive_thread_.joinable())
		msg_receive_thread_.join();

	delete ping_text_;
	delete gui_;
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
					//ball.materials.push_back(mat);
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
}

void BallanceMMOClient::process_incoming_message(blcl::net::message<MsgType>& msg)
{
	switch (msg.header.id) {
		case MsgType::ServerPing: {
			GetLogger()->Info("On ServerPing");
			std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
			std::chrono::system_clock::time_point sent;
			msg >> sent;
			//GetLogger()->Info("%d", int(std::chrono::duration_cast<std::chrono::milliseconds>(now - sent).count()));
			auto lk = std::scoped_lock<std::mutex>(ping_char_mtx_);
			sprintf(ping_char_, "Ping: %02lld ms", std::chrono::duration_cast<std::chrono::milliseconds>(now - sent).count());
			
			break;
		}
		case MsgType::ServerMessage: {
			GetLogger()->Info("On broadcast Msg");

			uint32_t remote_id;
			msg >> remote_id;
			BallState msg_state;
			msg >> msg_state;
//#ifdef DEBUG
			GetLogger()->Info("%d, B: %d, (%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f, %.2f)",
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

			if (peer_balls_.find(remote_id) == peer_balls_.end()) {
				// If message comes from a new client, then init balls and set IC
				PeerState state;
				for (size_t i = 0; i < ball_name_to_idx_.size(); i++)
					state.balls[i] = init_spirit_ball(i, remote_id);
				state.current_ball = msg_state.type;
				//if (state.current_ball > 3)
					//state.current_ball = 0;
				peer_balls_.insert({ remote_id, std::move(state) });
				peer_balls_[remote_id].balls[msg_state.type]->Show(CKSHOW);
			}

			if (peer_balls_.size() == 0)
				return;
			uint32_t current_ball = peer_balls_[remote_id].current_ball;
			uint32_t new_ball = msg_state.type;
			if (current_ball != new_ball) {
				peer_balls_[remote_id].balls[current_ball]->Show(CKHIDE);
				peer_balls_[remote_id].balls[new_ball]->Show(CKSHOW);

				peer_balls_[remote_id].current_ball = new_ball;
			}

			peer_balls_[remote_id].balls[new_ball]->SetPosition(msg_state.position);
			peer_balls_[remote_id].balls[new_ball]->SetQuaternion(msg_state.rotation);
			break;
		}
	}
}

CK3dObject* BallanceMMOClient::init_spirit_ball(int ball_index, uint32_t id) {
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
			//mat->EnableAlphaBlend();
			//mat->SetSourceBlend(VXBLEND_SRCALPHA);
			//mat->SetDestBlend(VXBLEND_INVSRCALPHA);
			//VxColor color = mat->GetDiffuse();
			//color.a = 0.5f;
			//mat->SetDiffuse(color);
			m_bml->SetIC(mat);
		}
	}
	return ball;
}