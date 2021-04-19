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
	player_ball_ = static_cast<CK3dObject*>(m_bml->GetArrayByName("CurrentLevel")->GetElementObject(0, 1));
}

void BallanceMMOClient::OnUnload()
{
	client_.disconnect();
}

void BallanceMMOClient::OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj)
{
	if (!strcmp(filename, "3D Entities\\Balls.nmo")) {
		CKDataArray* physBall = m_bml->GetArrayByName("Physicalize_GameBall");
		for (int i = 0; i < physBall->GetRowCount(); i++) {
			SpiritBall ball;
			std::string ball_name;
			ball_name.resize(physBall->GetElementStringValue(i, 0, nullptr), '\0');
			physBall->GetElementStringValue(i, 0, &ball_name[0]);
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
}

//int GetCurrentBall() {
	//CKObject* ball = m_curLevel->GetElementObject(0, 1);
	//if (ball) {
		//std::string ballName = ball->GetName();
		//for (size_t i = 0; i < m_dualBalls.size(); i++) {
			//if (m_dualBalls[i].name == ballName)
				//return i;
		//}
	//}

	//return 0;
//}
