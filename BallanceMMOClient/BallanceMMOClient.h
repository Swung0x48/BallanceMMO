#pragma once

#include <BML/BMLAll.h>
#include "client.h"
#include "CommandMMO.h"
#include "text_sprite.h"
#include "label_sprite.h"
#include <unordered_map>
#include <mutex>
#include <memory>

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class BallanceMMOClient : public IMod {
public:
	BallanceMMOClient(IBML* bml) : IMod(bml),
		client_([this](ammo::common::owned_message<PacketType>& msg) { OnMessage(msg); })
	{}

	virtual CKSTRING GetID() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetVersion() override { return "2.0.0-alpha1"; }
	virtual CKSTRING GetName() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetAuthor() override { return "Swung0x48"; }
	virtual CKSTRING GetDescription() override { return "The client to connect your game to the universe."; }
	DECLARE_BML_VERSION;

private:
	void OnLoad() override;
	void OnPostStartMenu() override;
	void OnExitGame() override;
	void OnUnload() override;
	void OnProcess() override;
	void OnStartLevel() override;
	void OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj) override;
	
	// Custom
	void OnMessage(ammo::common::owned_message<PacketType>& msg);
	void OnPeerTrafo(uint64_t id, int from, int to);

	std::unordered_map<std::string, IProperty*> props_;
	std::mutex bml_mtx_;
	std::mutex client_mtx_;
	client client_;

	const float RIGHT_MOST = 0.98f;

	bool init_ = false;
	uint64_t id_;
	std::shared_ptr<text_sprite> ping_;
	std::shared_ptr<text_sprite> status_;

	CK3dObject* player_ball_ = nullptr;
	struct BallState {
		uint32_t type = 0;
		VxVector position;
		VxQuaternion rotation;
	} ball_state_;
	std::vector<CK3dObject*> template_balls_;
	std::unordered_map<std::string, uint32_t> ball_name_to_idx_;
	CKDataArray* current_level_array_ = nullptr;

	struct PeerState {
		std::vector<CK3dObject*> balls;
		uint32_t current_ball = 0;
		std::string player_name = "";
		std::unique_ptr<label_sprite> username_label;
	};
	std::mutex peer_mtx_;
	std::unordered_map<uint64_t, PeerState> peer_balls_;
	CK3dObject* get_current_ball() {
		if (current_level_array_)
			return static_cast<CK3dObject*>(current_level_array_->GetElementObject(0, 1));

		return nullptr;
	}

	CK3dObject* init_spirit_ball(int ball_index, uint64_t id) {
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

	void init_spirit_balls(uint64_t id) {
		for (size_t i = 0; i < template_balls_.size(); ++i) {
			peer_balls_[id].balls[i] = init_spirit_ball(i, id);
		}
	}
};