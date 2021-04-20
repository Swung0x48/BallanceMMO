#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
	return new BallanceMMOClient(bml);
}

void BallanceMMOClient::OnLoad()
{
	client_.connect("192.168.50.100", 60000);
}

void BallanceMMOClient::OnPostStartMenu() {
	if (client_.is_connected())
		m_bml->SendIngameMessage("Connected!");
}

void BallanceMMOClient::OnProcess()
{
	if (m_bml->IsPlaying()) {
		auto* ball = get_current_ball();
		if (strcmp(ball->GetName(), player_ball_->GetName()) != 0) {
			// OnTrafo
			spirit_ball_ = spirit_balls_[ball->GetName()].obj;
			VxVector vec;
			ball->GetPosition(&vec);
			spirit_ball_->SetPosition(vec);
			spirit_ball_->Show(CKSHOW);
			//

			player_ball_ = ball;
		}

		player_ball_->GetPosition(&position_);
		player_ball_->GetQuaternion(&rotation_);
		msg_.clear();
		msg_.header.id = MsgType::MessageAll;
		msg_ << position_ << rotation_;
		client_.broadcast_message(msg_);
	}
}

void BallanceMMOClient::OnStartLevel()
{
	player_ball_ = get_current_ball();
	spirit_ball_ = spirit_balls_[player_ball_->GetName()].obj;
	VxVector vec(42, 15, -153);
	spirit_ball_->SetPosition(vec);
	spirit_ball_->Show(CKSHOW);
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
			SpiritBall ball;
			std::string ball_name;
			ball_name.resize(physicalized_ball->GetElementStringValue(i, 0, nullptr), '\0');
			physicalized_ball->GetElementStringValue(i, 0, &ball_name[0]);
			ball_name.pop_back();
			ball.obj = m_bml->Get3dObjectByName(ball_name.c_str());

			CKDependencies dep;
			dep.Resize(40); dep.Fill(0);
			dep.m_Flags = CK_DEPENDENCIES_CUSTOM;
			dep[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
			dep[CKCID_MESH] = CK_DEPENDENCIES_COPY_MESH_MATERIAL;
			dep[CKCID_3DENTITY] = CK_DEPENDENCIES_COPY_3DENTITY_MESH;
			ball.obj = static_cast<CK3dObject*>(m_bml->GetCKContext()->CopyObject(ball.obj, &dep, "_Spirit"));
			for (int j = 0; j < ball.obj->GetMeshCount(); j++) {
				CKMesh* mesh = ball.obj->GetMesh(j);
				for (int k = 0; k < mesh->GetMaterialCount(); k++) {
					CKMaterial* mat = mesh->GetMaterial(k);
					mat->EnableAlphaBlend();
					mat->SetSourceBlend(VXBLEND_SRCALPHA);
					mat->SetDestBlend(VXBLEND_INVSRCALPHA);
					VxColor color = mat->GetDiffuse();
					color.a = 0.5f;
					mat->SetDiffuse(color);
					ball.materials.push_back(mat);
					m_bml->SetIC(mat);
				}
			}
			spirit_balls_[ball_name] = ball;
		}
	}

	if (strcmp(filename, "3D Entities\\Gameplay.nmo") == 0) {
		current_level_array_ = m_bml->GetArrayByName("CurrentLevel");
	}
}

void BallanceMMOClient::process_incoming_message(const blcl::net::owned_message<MsgType>& msg)
{

}