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
	int opacity_counter = 0;
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
	game_objects(IBML* bml, game_state& db, std::function<CK3dObject*()> get_own_ball_fn):
			bml_(bml), db_(db), get_own_ball_fn_(get_own_ball_fn) {
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

	void update(SteamNetworkingMicroseconds timestamp, bool update_extra_info = false) {
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

    VxVector own_ball_pos;
    get_own_ball()->GetPosition(&own_ball_pos);

#if defined(DEBUG) || defined(BMMO_NAME_LABEL_WITH_EXTRA_INFO)
		static SteamNetworkingMicroseconds last_time_variance_update = 0;
		static bool update_time_variance = false;
		if (timestamp - last_time_variance_update >= 1000000) {
			last_time_variance_update = timestamp;
			update_time_variance = true;
		}
#endif

		db_.for_each([=, this, &viewport, &rc](const std::pair<const HSteamNetConnection, PlayerState>& item) {
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

			float square_ball_distance = 0;

			// Update ball states with togglable quadratic extrapolation
			if (!player.physicalized) {
#if defined(DEBUG) || defined(BMMO_NAME_LABEL_WITH_EXTRA_INFO)
				if (update_time_variance) {
					username_label->update(item.second.name + (item.second.cheated ? " [C]" : "") + " " + std::to_string(item.second.time_variance / 100000));
				}
#endif
				if (extrapolation_
						&& item.second.time_variance < MAX_EXTRAPOLATION_TIME_VARIANCE
						&& (state_it[0].position - state_it[1].position).SquareMagnitude() < MAX_EXTRAPOLATION_SQUARE_DISTANCE
						&& (state_it[1].position - state_it[2].position).SquareMagnitude() < MAX_EXTRAPOLATION_SQUARE_DISTANCE
				) {
					SteamNetworkingMicroseconds tc = timestamp;
					if (state_it->timestamp + MAX_EXTRAPOLATION_TIME < timestamp)
						tc = state_it->timestamp + MAX_EXTRAPOLATION_TIME;
					const auto& [position, rotation] = (item.second.time_variance > MAX_EXTRAPOLATION_TIME_VARIANCE / 2)
						? PlayerState::get_quadratic_extrapolated_state(tc, state_it[2], state_it[1], state_it[0])
						: PlayerState::get_linear_extrapolated_state(tc, state_it[1], state_it[0]);
					current_ball->SetPosition(VT21_REF(position));
					current_ball->SetQuaternion(VT21_REF(rotation));
					square_ball_distance = (position - own_ball_pos).SquareMagnitude();
				}
				else {
					current_ball->SetPosition(VT21_REF(state_it->position));
					current_ball->SetQuaternion(VT21_REF(state_it->rotation));
					square_ball_distance = (state_it->position - own_ball_pos).SquareMagnitude();
				}
			}

			if (dynamic_opacity_
#ifdef BMMO_WITH_PLAYER_SPECTATION
				&& spectated_id_ != item.first
#endif
			) {
				auto new_opacity = std::clamp(std::sqrt(square_ball_distance) * ALPHA_DISTANCE_RATE + ALPHA_BEGIN, ALPHA_MIN, ALPHA_MAX);
				if (std::fabsf(new_opacity - player.last_opacity) > 0.015625f || player.opacity_counter > 256) {
					// TODO: count if any of other balls are within the range of 2.0f
					int counter = 0;
					db_.for_each([&, this](const std::pair<const HSteamNetConnection, PlayerState>& item2) {
						if (item2.first == db_.get_client_id()) return true;
						const auto square_distance = (state_it->position).SquareMagnitude();
						if (square_distance < 16.0f) ++counter;
						return true;
					});
          bmmo::Printf("Name: %s, Counter: %d\n", item.second.name, counter);
					new_opacity /= std::max(1, counter);
					player.last_opacity = new_opacity;
					auto* current_material = static_cast<CKMaterial*>(bml_->GetCKContext()->GetObject(player.materials[current_ball_type]));
					VxColor color = current_material->GetDiffuse();
					color.a = new_opacity;
					current_material->SetDiffuse(color);
					player.opacity_counter = 0;
				}
				else
					++player.opacity_counter;
			}

			// Update username label
			if (update_extra_info) {
				username_label->update(get_username_label_text(item.second.name, item.second.cheated, item.second.ping));
			}
			VxRect extent; current_ball->GetRenderExtents(extent);
			if (isnan(extent.left) || !current_ball->IsInViewFrustrum(rc)) { // This player goes out of sight
				username_label->set_visible(false);
				return true;
			}
			// Vx2DVector pos((extent.left + extent.right) / 2.0f / viewport.right, (extent.top + extent.bottom) / 2.0f / viewport.bottom);
			if (!ball_type_changed)
				username_label->set_position({ extent.GetCenter() / viewport.GetBottomRight() });
			if (username_label->visible_ != db_.is_nametag_visible()) {
				username_label->set_visible(db_.is_nametag_visible());
				if (db_.is_nametag_visible())
					username_label->update(get_username_label_text(item.second.name, item.second.cheated, item.second.ping));
			}
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
private:
	int spectated_id_ = k_HSteamNetConnection_Invalid;

public:
	inline const int get_spectated_id() const { return spectated_id_; }

	inline void set_spectated_id(HSteamNetConnection id) {
		if (auto spectated_player = db_.get(spectated_id_); spectated_player.has_value()) {
			/* for (auto& mat_id : objects_[spectated_id_].materials) {
				/auto mat = static_cast<CKMaterial*>(bml_->GetCKContext()->GetObject(mat_id));
				// bml_->RestoreIC(mat); // that doesn't work
				mat->EnableAlphaBlend(true); // a little problematic too
				mat->SetSourceBlend(VXBLEND_SRCALPHA);
				mat->SetDestBlend(VXBLEND_INVSRCALPHA);
			} */
			// better to just be safe here.
			objects_.erase(spectated_id_);
			// don't wait until the next frame;
			// what if the player tries to spectate it again immediately?
			init_player(spectated_id_, spectated_player.value().name, spectated_player.value().cheated);
		}
		if (!db_.exists(id)) {
			spectated_id_ = k_HSteamNetConnection_Invalid;
			return;
		}
		spectated_id_ = id;
		for (auto& mat_id : objects_[spectated_id_].materials) {
			auto mat = static_cast<CKMaterial*>(bml_->GetCKContext()->GetObject(mat_id));
			mat->EnableAlphaBlend(false);
		}
	}

	inline const VxVector get_ball_pos(HSteamNetConnection id) {
		if (!db_.exists(id))
			return {};
		auto* player_ball = static_cast<CK3dObject*>(bml_->GetCKContext()->GetObject(
			objects_[id].balls[db_.get(id).value().ball_state.front().type]));
		VxVector pos;
		player_ball->GetPosition(&pos);
		return pos;
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

	std::string get_username_label_text(const std::string& name, bool cheated, uint16_t ping) const {
		std::string label_text = name + (cheated ? " [C]" : "");
		if (db_.is_ping_visible())
			label_text += std::format(" [{}ms]", ping);
		return label_text;
	};

	CK3dObject* get_own_ball() {
		return get_own_ball_fn_();
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
	std::function<CK3dObject* ()> get_own_ball_fn_;
	game_state& db_;
	std::unordered_map<HSteamNetConnection, PlayerObjects> objects_;
	bool extrapolation_ = false, dynamic_opacity_ = true;
	static constexpr SteamNetworkingMicroseconds MAX_EXTRAPOLATION_TIME = 163840;
	static constexpr float MAX_EXTRAPOLATION_SQUARE_DISTANCE = 512.0f;
	static constexpr int64_t MAX_EXTRAPOLATION_TIME_VARIANCE = 268435456ll;
	static constexpr float
		ALPHA_DEFAULT = 0.5f, ALPHA_DISTANCE_RATE = 0.0144f,
		ALPHA_BEGIN = 0.2f, ALPHA_MIN = 0.28f, ALPHA_MAX = 0.7f;
};
