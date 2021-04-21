#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
	return new BallanceMMOClient(bml);
}

void BallanceMMOClient::OnLoad()
{
	client_.connect("10.210.150.25", 60000);
}

void BallanceMMOClient::OnPostStartMenu() {
	if (client_.is_connected())
		m_bml->SendIngameMessage("Connected!");
}

void BallanceMMOClient::OnProcess()
{
	if (m_bml->IsPlaying()) {
		auto ball = get_current_ball();
		if (strcmp(ball->GetName(), player_ball_->GetName()) != 0) {
			// OnTrafo
			spirit_ball_ = template_balls_[ball_name_to_idx_[ball->GetName()[5]]];
			
			VxVector vec;
			ball->GetPosition(&vec);
			spirit_ball_->SetPosition(vec);
			spirit_ball_->Show(CKSHOW);
			
			// Update current player ball
			player_ball_ = ball;
			ball_status_.type = ball_name_to_idx_[player_ball_->GetName()[5]];
		}

		player_ball_->GetPosition(&ball_status_.position);
		player_ball_->GetQuaternion(&ball_status_.rotation);
		msg_.clear();
		msg_.header.id = MsgType::MessageAll;
		msg_ << ball_status_;
		client_.broadcast_message(msg_);
	}
}

void BallanceMMOClient::OnStartLevel()
{
	player_ball_ = get_current_ball();
	spirit_ball_ = template_balls_[ball_name_to_idx_[player_ball_->GetName()[5]]];
	ball_status_.type = ball_name_to_idx_[player_ball_->GetName()[5]];
	VxVector vec(42, 15, -153);
	spirit_ball_->SetPosition(vec);
	spirit_ball_->Show(CKSHOW);

	msg_receive_thread_ = std::thread([this]() {
		while (m_bml->IsIngame())
		{
			while (client_.get_incoming_messages().empty())
				client_.get_incoming_messages().wait();

			auto msg = client_.get_incoming_messages().pop_front();
			process_incoming_message(msg.msg);
		}
	});
}

void BallanceMMOClient::OnUnload()
{
	client_.disconnect();
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
			ball = static_cast<CK3dObject*>(m_bml->GetCKContext()->CopyObject(ball, &dep, "_Spirit_"));
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
			ball_name_to_idx_[ball_name[5]] = i; // "Ball_Xxx" 
												 //		  ^
		}
	}

	if (strcmp(filename, "3D Entities\\Gameplay.nmo") == 0) {
		current_level_array_ = m_bml->GetArrayByName("CurrentLevel");
	}
}

void BallanceMMOClient::process_incoming_message(blcl::net::message<MsgType>& msg)
{
	switch (msg.header.id) {
		case MsgType::ServerMessage: {
			m_bml->SendIngameMessage("Incoming message!");

			uint32_t remote_id;
			msg >> remote_id;
			BallState msg_state;
			msg >> msg_state;

			GetLogger()->Info("%d, (%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f, %.2f)",
				remote_id,
				msg_state.position.x,
				msg_state.position.y,
				msg_state.position.z,
				msg_state.rotation.x,
				msg_state.rotation.y,
				msg_state.rotation.z,
				msg_state.rotation.w);
			if (peer_balls_.find(remote_id) == peer_balls_.end()) {
				// If message comes from a new client, then init balls and set IC
				PeerState state;
				for (size_t i = 0; i < 3; i++)
					state.balls[i] = init_spirit_ball(i, remote_id);
				state.current_ball = msg_state.type;
				peer_balls_[remote_id] = state;
			}

			peer_balls_[remote_id].balls[msg_state.type]->SetPosition(msg_state.position);
			peer_balls_[remote_id].balls[msg_state.type]->SetQuaternion(msg_state.rotation);
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