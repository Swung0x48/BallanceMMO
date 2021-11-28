#pragma once
#include <BML/BMLAll.h>
#include "label_sprite.h"
#include "game_state.h"
#include <unordered_map>

struct PlayerObjects {
	static inline IBML* bml;
	std::vector<CK3dObject*> balls;
	std::unique_ptr<label_sprite> username_label;
	uint32_t visible_ball_type = 0;
	bool physicalized = false;

	~PlayerObjects() {
		for (auto* ball: balls)
			bml->GetCKContext()->DestroyObject(ball);

		username_label.reset();
	}
};

class game_objects {
public:
	game_objects(IBML* bml, game_state& db): bml_(bml), db_(db) {
		PlayerObjects::bml = bml;
	}

	void init_player(const HSteamNetConnection id, const std::string& name) {
		auto& obj = objects_[id];
		for (int i = 0; i < template_balls_.size(); ++i) {
			obj.balls.emplace_back(init_spirit_ball(i, id));
		}
		obj.username_label = std::make_unique<label_sprite>(
			"Name_" + name,
			name,
			0.5, 0.5);
	}

	void init_players() {
		db_.for_each([this](const std::pair<const HSteamNetConnection, PlayerState>& item) {
			if (item.first != db_.get_client_id())
				init_player(item.first, item.second.name);
			return true;
		});
	}

	void on_trafo(HSteamNetConnection id, uint32_t from, uint32_t to) {
		auto* old_ball = objects_[id].balls[from];
		auto* new_ball = objects_[id].balls[to];

		old_ball->Show(CKHIDE);
		new_ball->Show(CKSHOW);
	}

	void update() {
		// Can be costly
		//cleanup();

		// Can also be costly
		/*db_.for_each([this](const std::pair<const HSteamNetConnection, PlayerState>& item) {
			init_player(item.first, item.second.name);
			return true;
		});*/

		VxRect viewport; bml_->GetRenderContext()->GetViewRect(viewport);

		db_.for_each([this, &viewport](const std::pair<const HSteamNetConnection, PlayerState>& item) {
			if (item.first == db_.get_client_id())
				return true;

			if (!objects_.contains(item.first)) {
				init_player(item.first, item.second.name);
			}

			uint32_t current_ball_type = item.second.ball_state.type;

			if (current_ball_type != objects_[item.first].visible_ball_type) {
				on_trafo(item.first, objects_[item.first].visible_ball_type, current_ball_type);
				objects_[item.first].visible_ball_type = current_ball_type;
			}

			auto* current_ball = objects_[item.first].balls[current_ball_type];
			auto& username_label = objects_[item.first].username_label;

			// Update ball
			if (!objects_[item.first].physicalized) {
				current_ball->SetPosition(item.second.ball_state.position);
				current_ball->SetQuaternion(item.second.ball_state.rotation);
			}

			// Update username label
			VxRect extent; objects_[item.first].balls[current_ball_type]->GetRenderExtents(extent);
			Vx2DVector pos((extent.left + extent.right) / 2.0f / viewport.right, extent.top / viewport.bottom);
			username_label->set_position(pos);
			username_label->set_visible(true);
			username_label->process();
			return true;
		});
	}

	void physicalize(HSteamNetConnection id) {
		CKDataArray* physBall = bml_->GetArrayByName("Physicalize_GameBall");

		uint32_t current_ball_type = db_.get(id)->ball_state.type;
		auto* current_ball = objects_[id].balls[current_ball_type];
		objects_[id].physicalized = true;
		std::string ballName(physBall->GetElementStringValue(current_ball_type, 0, nullptr), '\0');
		physBall->GetElementStringValue(current_ball_type, 0, ballName.data());
		ballName += "_Mesh";
		ExecuteBB::PhysicalizeBall(current_ball, false, 0.7f, 0.4f, 1.0f, "", false, true, true, 0.1f, 0.1f, ballName.c_str());
	}

	void physicalize_all() {
		for (const auto& i : objects_) {
			physicalize(i.first);
		}
	}

	void remove(HSteamNetConnection id) {
		objects_.erase(id);
	}

	void purge_dead() {
		std::erase_if(objects_, [this](const auto& item) {
			auto const& [key, value] = item;
			return !db_.exists(key);
		});
	}

	void init_template_balls() {
		CKDataArray* physicalized_ball = bml_->GetArrayByName("Physicalize_GameBall");

		template_balls_.reserve(physicalized_ball->GetRowCount());
		for (int i = 0; i < physicalized_ball->GetRowCount(); i++) {
			CK3dObject* ball;
			std::string ball_name;
			ball_name.resize(physicalized_ball->GetElementStringValue(i, 0, nullptr), '\0');
			physicalized_ball->GetElementStringValue(i, 0, &ball_name[0]);
			ball_name.pop_back();
			ball = bml_->Get3dObjectByName(ball_name.c_str());

			CKDependencies dep;
			dep.Resize(40); dep.Fill(0);
			dep.m_Flags = CK_DEPENDENCIES_CUSTOM;
			dep[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
			dep[CKCID_MESH] = CK_DEPENDENCIES_COPY_MESH_MATERIAL;
			dep[CKCID_3DENTITY] = CK_DEPENDENCIES_COPY_3DENTITY_MESH;
			ball = static_cast<CK3dObject*>(bml_->GetCKContext()->CopyObject(ball, &dep, "_Peer_"));
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
					bml_->SetIC(mat);
				}
			}
			template_balls_.emplace_back(ball);
			db_.set_ball_id(ball_name, i);  // "Ball_Xxx"
		}
	}

	CK3dObject* init_spirit_ball(int ball_index, uint64_t id) {
		CKDependencies dep;
		dep.Resize(40); dep.Fill(0);
		dep.m_Flags = CK_DEPENDENCIES_CUSTOM;
		dep[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
		dep[CKCID_MESH] = CK_DEPENDENCIES_COPY_MESH_MATERIAL;
		dep[CKCID_3DENTITY] = CK_DEPENDENCIES_COPY_3DENTITY_MESH;
		CK3dObject* ball = static_cast<CK3dObject*>(bml_->GetCKContext()->CopyObject(template_balls_[ball_index], &dep, std::to_string(id).c_str()));
		for (int j = 0; j < ball->GetMeshCount(); j++) {
			CKMesh* mesh = ball->GetMesh(j);
			for (int k = 0; k < mesh->GetMaterialCount(); k++) {
				CKMaterial* mat = mesh->GetMaterial(k);
				bml_->SetIC(mat);
			}
		}

		return ball;
	}

	void destroy_all_objects() {
		objects_.clear();
	}

	void destroy_templates() {
		for (auto* i : template_balls_) {
			bml_->GetCKContext()->DestroyObject(i);
		}
	}

	~game_objects() {
		destroy_all_objects();
		destroy_templates();
	}
private:
	std::vector<CK3dObject*> template_balls_;
	IBML* bml_ = nullptr;
	game_state& db_;
	std::unordered_map<HSteamNetConnection, PlayerObjects> objects_;
};
