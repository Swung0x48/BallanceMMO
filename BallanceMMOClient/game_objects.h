#pragma once
#include "bml_includes.h"
#include "label_sprite.h"
#include "game_state.h"
#include <unordered_map>

struct PlayerObjects {
	static inline IBML* bml;
	std::vector<CK_ID> balls;
	std::vector<CK_ID> materials;
	std::unique_ptr<label_sprite> username_label;
	uint32_t visible_ball_type = std::numeric_limits<decltype(visible_ball_type)>::max();
	float last_opacity = 0.5;
	bool physicalized = false;

	~PlayerObjects() {
		for (auto ball : balls) {
			auto* ball_ptr = bml->GetCKContext()->GetObject(ball);
			if (ball_ptr)
				bml->GetCKContext()->DestroyObject(ball);
		}

		username_label.reset();
	}
};

class game_objects {
public:
	game_objects(IBML* bml, game_state& db): bml_(bml), db_(db) {
		PlayerObjects::bml = bml;
	}

	void init_player(const HSteamNetConnection id, const std::string& name, bool cheat) {
		auto& obj = objects_[id];
		for (size_t i = 0; i < template_balls_.size(); ++i) {
			auto* ball = init_spirit_ball(i, id);
			if (ball) {
				obj.balls.emplace_back(CKOBJID(ball));
				if (ball->GetMeshCount() > 0 && ball->GetMesh(0)->GetMaterialCount() > 0)
					obj.materials.emplace_back(CKOBJID(ball->GetMesh(0)->GetMaterial(0)));
			}
			else {
				auto msg = std::format("Failed to init player: {} {}", id, i);
				bml_->SendIngameMessage(msg.c_str());
			}
		}
		obj.username_label = std::make_unique<label_sprite>(
			"Name_" + name,
			name + (cheat ? " [C]" : ""),
			0.5f, 0.5f);
	}

	void init_players() {
		db_.for_each([this](const std::pair<const HSteamNetConnection, PlayerState>& item) {
			if (item.first != db_.get_client_id())
				init_player(item.first, item.second.name, item.second.cheated);
			return true;
		});
	}

	void on_trafo(HSteamNetConnection id, uint32_t from, uint32_t to) {
		auto* old_ball = bml_->GetCKContext()->GetObject(objects_[id].balls[from]);
		auto* new_ball = bml_->GetCKContext()->GetObject(objects_[id].balls[to]);

		assert(old_ball && new_ball);
		if (old_ball)
			old_ball->Show(CKHIDE);

		if (new_ball)
			new_ball->Show(CKSHOW);
	}

	void update(SteamNetworkingMicroseconds timestamp, bool update_cheat = false) {
		// Can be costly
		//cleanup();

		// Can also be costly
		/*db_.for_each([this](const std::pair<const HSteamNetConnection, PlayerState>& item) {
			init_player(item.first, item.second.name);
			return true;
		});*/

		auto* rc = bml_->GetRenderContext();
		if (!rc)
			return;
		VxRect viewport; rc->GetViewRect(viewport);

		VxVector camera_target_pos{};
		auto* camera = rc->GetAttachedCamera();
		if (camera) {
			camera->GetPosition(&camera_target_pos);
			VxVector orientation;
			camera->GetOrientation(&orientation, nullptr);
			camera_target_pos += orientation * CAMERA_TARGET_DISTANCE;
		}

		db_.for_each([=, this, &viewport, &rc](std::pair<const HSteamNetConnection, PlayerState>& item) {
			// Not creating or updating game object for this client itself.
			//if (item.first == db_.get_client_id())
			//	return true;

			if (!objects_.contains(item.first)) {
				init_player(item.first, item.second.name, item.second.cheated);
			}

			auto& player = objects_[item.first];
			const auto& state_it = item.second.ball_state.begin();

			const uint32_t current_ball_type = state_it->type;
			const bool ball_type_changed = (current_ball_type != player.visible_ball_type);

			if (ball_type_changed) {
				on_trafo(item.first, player.visible_ball_type, current_ball_type);
				player.visible_ball_type = current_ball_type;
			}

			/*bml_->SendIngameMessage(std::to_string(item.first).c_str());
			bml_->SendIngameMessage(std::to_string(current_ball_type).c_str());*/

			auto* current_ball = static_cast<CK3dObject*>(bml_->GetCKContext()->GetObject(player.balls[current_ball_type]));
			auto& username_label = player.username_label;

			if (current_ball == nullptr || username_label == nullptr) // Maybe a client quit unexpectedly.
				return true;

			float square_camera_distance = 0;

			// Update ball states with togglable quadratic extrapolation
			if (!player.physicalized) {
#if defined(DEBUG) || defined(BMMO_NAME_LABEL_WITH_EXTRA_INFO)
				item.second.counter++;
				if (item.second.counter % 66 == 0) {
					username_label->update(item.second.name + (item.second.cheated ? " [C]" : "") + " " + std::to_string(item.second.time_variance / 100000));
				}
#endif
				if (extrapolation_ && [=, this]() mutable {
					if ((state_it[0].position - state_it[1].position).SquareMagnitude() < MAX_EXTRAPOLATION_SQUARE_DISTANCE
							&& item.second.time_variance < MAX_EXTRAPOLATION_TIME_VARIANCE)
						return true;
					item.second.ball_state.push_front(state_it[0]);
					item.second.ball_state.push_front(state_it[0]);
					return false;
				}()) {
					SteamNetworkingMicroseconds tc = timestamp;
					if (state_it->timestamp + MAX_EXTRAPOLATION_TIME < timestamp)
						tc = state_it->timestamp + MAX_EXTRAPOLATION_TIME;
					const auto& [position, rotation] = (item.second.time_variance > MAX_EXTRAPOLATION_TIME_VARIANCE / 2)
						? PlayerState::get_quadratic_extrapolated_state(tc, state_it[2], state_it[1], state_it[0])
						: PlayerState::get_linear_extrapolated_state(tc, state_it[1], state_it[0]);
					current_ball->SetPosition(position);
					current_ball->SetQuaternion(rotation);
					square_camera_distance = (position - camera_target_pos).SquareMagnitude();
				}
				else {
					current_ball->SetPosition(state_it->position);
					current_ball->SetQuaternion(state_it->rotation);
					square_camera_distance = (state_it->position - camera_target_pos).SquareMagnitude();
				}
			}

			if (dynamic_opacity_) {
				const auto new_opacity = std::clamp(std::sqrt(square_camera_distance) * ALPHA_DISTANCE_RATE + ALPHA_BEGIN, ALPHA_MIN, ALPHA_MAX);
				if (std::fabsf(new_opacity - player.last_opacity) > 0.015625f) {
					player.last_opacity = new_opacity;
					auto* current_material = static_cast<CKMaterial*>(bml_->GetCKContext()->GetObject(player.materials[current_ball_type]));
					VxColor color = current_material->GetDiffuse();
					color.a = new_opacity;
					current_material->SetDiffuse(color);
				}
			}

			// Update username label
			if (update_cheat) {
				username_label->update(item.second.name + (item.second.cheated ? " [C]" : ""));
			}
			VxRect extent; current_ball->GetRenderExtents(extent);
			if (isnan(extent.left) || !current_ball->IsInViewFrustrum(rc)) { // This player goes out of sight
				username_label->set_visible(false);
				return true;
			}
			// Vx2DVector pos((extent.left + extent.right) / 2.0f / viewport.right, (extent.top + extent.bottom) / 2.0f / viewport.bottom);
			if (!ball_type_changed)
				username_label->set_position({ extent.GetCenter() / viewport.GetBottomRight() });
			if (username_label->visible_ != db_.is_nametag_visible())
				username_label->set_visible(db_.is_nametag_visible());
			username_label->process();
			return true;
		});
	}

	void physicalize(HSteamNetConnection id) {
		CKDataArray* physBall = bml_->GetArrayByName("Physicalize_GameBall");

		uint32_t current_ball_type = db_.get(id)->ball_state.front().type;
		auto* current_ball = static_cast<CK3dObject*>(bml_->GetCKContext()->GetObject(objects_[id].balls[current_ball_type]));
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
			physicalized_ball->GetElementStringValue(i, 0, ball_name.data());
			ball_name.pop_back();
			ball = bml_->Get3dObjectByName(ball_name.data());

			CKDependencies dep;
			dep.Resize(40); dep.Fill(0);
			dep.m_Flags = CK_DEPENDENCIES_CUSTOM;
			dep[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
			dep[CKCID_MESH] = CK_DEPENDENCIES_COPY_MESH_MATERIAL;
			dep[CKCID_3DENTITY] = CK_DEPENDENCIES_COPY_3DENTITY_MESH;
			ball = static_cast<CK3dObject*>(bml_->GetCKContext()->CopyObject(ball, &dep, "_Peer_"));
			if (!ball) {
				bml_->SendIngameMessage(std::format("Failed to init template ball: {}", i).c_str());
				continue;
			}
			assert(ball->GetMeshCount() == 1);
			for (int j = 0; j < ball->GetMeshCount(); j++) {
				CKMesh* mesh = ball->GetMesh(j);
				assert(mesh->GetMaterialCount() >= 1);
				for (int k = 0; k < mesh->GetMaterialCount(); k++) {
					CKMaterial* mat = mesh->GetMaterial(k);
					mat->EnableAlphaBlend();
					mat->SetSourceBlend(VXBLEND_SRCALPHA);
					mat->SetDestBlend(VXBLEND_INVSRCALPHA);
					VxColor color = mat->GetDiffuse();
					color.a = ALPHA_DEFAULT;
					mat->SetDiffuse(color);
					bml_->SetIC(mat);
				}
			}
			template_balls_.emplace_back(CKOBJID(ball));
			db_.set_ball_id(ball_name, i);  // "Ball_Xxx"
		}
		template_init_ = true;
	}

	CK3dObject* init_spirit_ball(int ball_index, uint64_t id) {
		CKDependencies dep;
		dep.Resize(40); dep.Fill(0);
		dep.m_Flags = CK_DEPENDENCIES_CUSTOM;
		dep[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
		dep[CKCID_MESH] = CK_DEPENDENCIES_COPY_MESH_MATERIAL;
		dep[CKCID_3DENTITY] = CK_DEPENDENCIES_COPY_3DENTITY_MESH;
		auto* template_ball = bml_->GetCKContext()->GetObject(template_balls_[ball_index]);
		if (!template_ball) {
			auto msg = std::format("Failed to find template ball when initializing: {} {}", id, ball_index);
			bml_->SendIngameMessage(msg.c_str());
			return nullptr;
		}

		CK3dObject* ball = static_cast<CK3dObject*>(bml_->GetCKContext()->CopyObject(template_ball, &dep, std::to_string(id).data()));
		
		if (!ball) {
			auto msg = std::format("Failed to copy template ball when initializing: {} {}", id, ball_index);
			bml_->SendIngameMessage(msg.c_str());
			return nullptr;
		}

		for (int j = 0; j < ball->GetMeshCount(); j++) {
			CKMesh* mesh = ball->GetMesh(j);
			for (int k = 0; k < mesh->GetMaterialCount(); k++) {
				CKMaterial* mat = mesh->GetMaterial(k);
				bml_->SetIC(mat);
			}
		}

		return ball;
	}

#ifdef BMMO_WITH_PLAYER_SPECTATION
	inline const std::pair<VxVector, VxQuaternion> get_ball_state(HSteamNetConnection id) {
		if (!db_.exists(id))
			return {};
		auto* player_ball = static_cast<CK3dObject*>(bml_->GetCKContext()->GetObject(
			objects_[id].balls[db_.get(id).value().ball_state.front().type]));
		VxVector pos; VxQuaternion rot;
		player_ball->GetPosition(&pos);
		player_ball->GetQuaternion(&rot);
		return {pos, rot};
	}
#endif

	void toggle_extrapolation(bool enabled) { extrapolation_ = enabled; }

	void toggle_dynamic_opacity(bool enabled) {
		dynamic_opacity_ = enabled;
		if (enabled) return;
		db_.for_each([this](const std::pair<const HSteamNetConnection, PlayerState>& item) {
			auto player_it = objects_.find(item.first);
			if (player_it == objects_.end()) return true;
			for (const CK_ID mat_id : player_it->second.materials) {
				auto mat = static_cast<CKMaterial*>(bml_->GetCKContext()->GetObject(mat_id));
				VxColor color = mat->GetDiffuse();
				color.a = ALPHA_DEFAULT;
				mat->SetDiffuse(color);
			}
			return true;
		});
	}

	void destroy_all_objects() {
		objects_.clear();
	}

	void destroy_templates() {
		auto* ctx = bml_->GetCKContext();
		for (auto i : template_balls_) {
			auto* obj = ctx->GetObject(i);
			bml_->GetCKContext()->DestroyObject(obj);
		}
	}

	void reload() {
		destroy_all_objects();
		init_players();
	}

	~game_objects() {
		destroy_all_objects();
		destroy_templates();
	}
private:
	bool template_init_ = false;
	std::vector<CK_ID> template_balls_;
	IBML* bml_ = nullptr;
	game_state& db_;
	std::unordered_map<HSteamNetConnection, PlayerObjects> objects_;
	bool extrapolation_ = false, dynamic_opacity_ = true;
	static constexpr SteamNetworkingMicroseconds MAX_EXTRAPOLATION_TIME = 163840;
	static constexpr float MAX_EXTRAPOLATION_SQUARE_DISTANCE = 512.0f;
	static constexpr int64_t MAX_EXTRAPOLATION_TIME_VARIANCE = 268435456ll;
	static constexpr float CAMERA_TARGET_DISTANCE = 40.0f,
		ALPHA_DEFAULT = 0.5f, ALPHA_DISTANCE_RATE = 0.0144f,
		ALPHA_BEGIN = 0.2f, ALPHA_MIN = 0.28f, ALPHA_MAX = 0.7f;
};
