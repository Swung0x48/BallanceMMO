#pragma once
#include <mutex>
#include <unordered_map>
#include "bml_includes.h"
#include "label_sprite.h"
#include "text_sprite.h"
#include "game_state.h"
#include "utils.h"

struct PlayerObjects {
	static inline IBML* bml;
	std::vector<CK_ID> balls;
	std::vector<CK_ID> materials;
	std::unique_ptr<sprite_interface> username_label;
	uint32_t visible_ball_type = std::numeric_limits<decltype(visible_ball_type)>::max();
	float last_opacity = 0.5;
	SteamNetworkingMicroseconds last_opacity_timestamp = 0;
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
	game_objects(IBML* bml, game_state& db, std::function<CK3dObject*()> get_own_ball_fn, const utils* utils):
			bml_(bml), db_(db), get_own_ball_fn_(get_own_ball_fn), utils_(utils) {
		PlayerObjects::bml = bml;
	}

	void init_player(const HSteamNetConnection id, const std::string& name, bool cheat) {
		std::lock_guard lk(mutex_);
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
		if ([&name] { // test if not ascii
					for (const auto& c : name) {
						if (static_cast<unsigned char>(c) > 127)
							return true;
					}
					return false;
				}() || name.length() > 20) {
			// use text sprite for non-ascii or too long names
			auto username_label = std::make_unique<text_sprite>(
				"Name_" + name,
				name + (cheat ? " [C]" : ""),
				0.5f, 0.5f);
			username_label->sprite_->SetSize(Vx2DVector(0.3f, 0.03f));
			username_label->sprite_->SetAlignment(static_cast<CKSPRITETEXT_ALIGNMENT>(CKSPRITETEXT_LEFT | CKSPRITETEXT_VCENTER));
			username_label->sprite_->SetFont(utils::get_system_font(), utils_->get_display_font_size(9.1f), 500, false, false);
			username_label->paint(0xF9F9F9F9);
			obj.username_label = std::move(username_label);
		}
		else {
			obj.username_label = std::make_unique<label_sprite>(
				"Name_" + name,
				name + (cheat ? " [C]" : ""),
				0.5f, 0.5f);
		}
	}

	void init_players() {
		db_.for_each([this](const std::pair<const HSteamNetConnection, PlayerState>& item) {
			if (item.first != db_.get_client_id())
				init_player(item.first, item.second.name, item.second.cheated);
			return true;
		});
	}

	// Ball types arrive over the network, so they must never be used as an
	// index without checking: the ball list only has one entry per template.
	static bool is_valid_ball_type(const PlayerObjects& player, uint32_t type) {
		return type < player.balls.size();
	}

	// no lock here: we lock it within the caller function
	// we can use recursive_mutex here, but that's really only a dirty hack
	// it's better not to overly rely on that
	void on_trafo(HSteamNetConnection id, uint32_t from, uint32_t to) {
		auto& player = objects_[id];
		if (from != std::numeric_limits<decltype(from)>::max() && is_valid_ball_type(player, from)) {
			auto* old_ball = bml_->GetCKContext()->GetObject(player.balls[from]);
			if (old_ball)
				old_ball->Show(CKHIDE);
			assert(old_ball);
		}

		if (!is_valid_ball_type(player, to))
			return;
		auto* new_ball = bml_->GetCKContext()->GetObject(player.balls[to]);
		if (new_ball)
			new_ball->Show(CKSHOW);
		assert(new_ball);
	}

	bool update(SteamNetworkingMicroseconds timestamp, bool update_extra_info = false) {
		// Can be costly
		//cleanup();

		// Can also be costly
		/*db_.for_each([this](const std::pair<const HSteamNetConnection, PlayerState>& item) {
			init_player(item.first, item.second.name);
			return true;
		});*/

		auto* rc = bml_->GetRenderContext();
		if (!rc)
			return false;
		VxRect viewport; rc->GetViewRect(viewport);

		VxVector own_ball_pos;
		auto* own_ball = get_own_ball();
		if (!own_ball) // null between levels / during teardown
			return false;
		own_ball->GetPosition(&own_ball_pos);

		const auto* camera = rc->GetAttachedCamera();
		VxVector camera_pos = own_ball_pos;
		if (camera) camera->GetPosition(&camera_pos);

#if defined(DEBUG) || defined(BMMO_NAME_LABEL_WITH_EXTRA_INFO)
		static SteamNetworkingMicroseconds last_time_variance_update = 0;
		bool update_time_variance = false;
		if (timestamp - last_time_variance_update >= 1000000) {
			last_time_variance_update = timestamp;
			update_time_variance = true;
		}
#endif

		return db_.try_for_each_const([=, this, &viewport, &rc, &own_ball_pos, &camera_pos]
				(const std::pair<const HSteamNetConnection, PlayerState>& item) {
			// Not creating or updating game object for this client itself.
			//if (item.first == db_.get_client_id())
			//	return true;

			if (!objects_.contains(item.first)) {
				init_player(item.first, item.second.name, item.second.cheated);
			}

			std::unique_lock lk(mutex_, std::try_to_lock); // lock after init to avoid deadlock
			if (!lk) return true;

			auto& player = objects_[item.first];
			const auto& state_it = item.second.ball_state.begin();

			const uint32_t current_ball_type = state_it->type;
			// a peer (or a forged packet) can claim any ball type; skip this
			// player rather than indexing past the end of its ball list
			if (!is_valid_ball_type(player, current_ball_type))
				return true;
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

			float lateral_distance = 0;

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
					lateral_distance = utils::distance_to_line_segment((
#ifdef BMMO_WITH_PLAYER_SPECTATION
						spectated_id_ != k_HSteamNetConnection_Invalid ? get_ball_pos(spectated_id_) :
#endif
						own_ball_pos), camera_pos, position);
				}
				else {
					current_ball->SetPosition(VT21_REF(state_it->position));
					current_ball->SetQuaternion(VT21_REF(state_it->rotation));
					lateral_distance = utils::distance_to_line_segment((
#ifdef BMMO_WITH_PLAYER_SPECTATION
						spectated_id_ != k_HSteamNetConnection_Invalid ? get_ball_pos(spectated_id_) :
#endif
						own_ball_pos), camera_pos, state_it->position);
				}
			}

			if (dynamic_opacity_
				&& !player.physicalized // balls in the simulation stay solid
#ifdef BMMO_WITH_PLAYER_SPECTATION
				&& spectated_id_ != item.first
#endif
				&& (timestamp - player.last_opacity_timestamp >= 114688 || ball_type_changed) // 7 * 2^14
			) {
				auto new_opacity = std::clamp(lateral_distance * ALPHA_DISTANCE_RATE + ALPHA_BEGIN, ALPHA_MIN, ALPHA_MAX);
				float dilation_factor = 1.0f;
				db_.try_for_each_const([&, this](const std::pair<const HSteamNetConnection, PlayerState>& item2) {
					if (item2.first == db_.get_client_id() || item2.first == item.first
							|| bmmo::name_validator::is_spectator(item2.second.name))
						return true;
					const auto square_distance = (state_it->position - item2.second.ball_state.front().position).SquareMagnitude();
					if (square_distance < DILATION_MAX_SQUARE_DISTANCE)
						dilation_factor += 1.0f - square_distance / DILATION_MAX_SQUARE_DISTANCE;
					return true;
				});
				new_opacity = std::clamp(new_opacity * 3 / dilation_factor, 0.09375f, new_opacity);
				if (std::fabsf(new_opacity - player.last_opacity) > 0.015625f || ball_type_changed) {
					player.last_opacity = new_opacity;
					auto* current_material = static_cast<CKMaterial*>(bml_->GetCKContext()->GetObject(player.materials[current_ball_type]));
					VxColor color = current_material->GetDiffuse();
					color.a = new_opacity;
					current_material->SetDiffuse(color);
				}
				player.last_opacity_timestamp = timestamp;
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
			if (!ball_type_changed)
				username_label->set_position(extent.GetCenter() / viewport.GetBottomRight());
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
		if (!physBall) return;

		auto state = db_.get(id);
		if (!state) return;
		uint32_t current_ball_type = state->ball_state.front().type;
		auto& player = objects_[id];
		if (!is_valid_ball_type(player, current_ball_type)) return;
		auto* current_ball = static_cast<CK3dObject*>(bml_->GetCKContext()->GetObject(player.balls[current_ball_type]));
		if (!player.physicalized) {
			player.physicalized = true;
			set_ball_solid(id, player, true);
		}
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

	// Physics sessions: the spirit ball entity of a player's ball type, so the
	// bridge can give it a body mirrored from the server.
	CK3dObject* get_ball_entity(HSteamNetConnection id, uint32_t type) {
		std::lock_guard lk(mutex_);
		auto it = objects_.find(id);
		if (it == objects_.end() || !is_valid_ball_type(it->second, type)) return nullptr;
		return static_cast<CK3dObject*>(bml_->GetCKContext()->GetObject(it->second.balls[type]));
	}
	void ensure_player(HSteamNetConnection id) {
		std::lock_guard lk(mutex_);
		if (objects_.contains(id)) return;
		if (auto state = db_.get(id)) init_player(id, state->name, state->cheated);
	}
	// While set, update() leaves the ball where the physics body put it.
	void set_physicalized(HSteamNetConnection id, bool physicalized) {
		std::lock_guard lk(mutex_);
		auto it = objects_.find(id);
		if (it == objects_.end() || it->second.physicalized == physicalized) return;
		it->second.physicalized = physicalized;
		// A ball that takes part in the simulation is a solid object one can
		// collide with, so it looks like one; only shadow mode keeps peers
		// translucent (where they are just ghosts passing through).
		set_ball_solid(id, it->second, physicalized);
	}

	void remove(HSteamNetConnection id) {
		std::lock_guard lk(mutex_);
		objects_.erase(id);
	}

	void purge_dead() {
		std::lock_guard lk(mutex_);
		std::erase_if(objects_, [this](const auto& item) {
			auto const& [key, value] = item;
			return !db_.exists(key);
		});
	}

	void init_template_balls() {
		std::lock_guard lk(mutex_);
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
		std::lock_guard lk(mutex_);
		spectated_id_ = id;
		for (auto& mat_id : objects_[spectated_id_].materials) {
			auto mat = static_cast<CKMaterial*>(bml_->GetCKContext()->GetObject(mat_id));
			mat->EnableAlphaBlend(false);
		}
	}

	inline const VxVector get_ball_pos(HSteamNetConnection id) {
		auto state = db_.get(id);
		if (!state)
			return {};
		auto player_it = objects_.find(id);
		if (player_it == objects_.end())
			return {};
		const uint32_t ball_type = state->ball_state.front().type;
		if (!is_valid_ball_type(player_it->second, ball_type))
			return {};
		auto* player_ball = static_cast<CK3dObject*>(bml_->GetCKContext()->GetObject(
			player_it->second.balls[ball_type]));
		if (!player_ball)
			return {};
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
			if (player_it == objects_.end() || player_it->second.physicalized) return true;
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
		std::lock_guard lk(mutex_);
		objects_.clear();
	}

	void destroy_templates() {
		std::lock_guard lk(mutex_);
		auto* ctx = bml_->GetCKContext();
		for (auto i : template_balls_) {
			auto* obj = ctx->GetObject(i);
			bml_->GetCKContext()->DestroyObject(obj);
		}
	}

	std::string get_username_label_text(const std::string& name, bool cheated, uint16_t ping) const {
		std::string label_text = bmmo::string_utils::utf8_to_ansi(name) + (cheated ? " [C]" : "");
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
	// Solid balls (physics sessions) drop alpha blending altogether; shadow
	// mode balls go back to the translucent default, with the dynamic opacity
	// timer reset so update() picks a distance-based value right away.
	void set_ball_solid(HSteamNetConnection id, PlayerObjects& player, bool solid) {
#ifdef BMMO_WITH_PLAYER_SPECTATION
		if (!solid && spectated_id_ == id) return; // spectated balls stay solid
#else
		(void)id;
#endif
		for (const CK_ID mat_id : player.materials) {
			auto* mat = static_cast<CKMaterial*>(bml_->GetCKContext()->GetObject(mat_id));
			if (!mat) continue;
			VxColor color = mat->GetDiffuse();
			color.a = solid ? 1.0f : ALPHA_DEFAULT;
			mat->SetDiffuse(color);
			mat->EnableAlphaBlend(!solid);
			if (!solid) {
				mat->SetSourceBlend(VXBLEND_SRCALPHA);
				mat->SetDestBlend(VXBLEND_INVSRCALPHA);
			}
		}
		player.last_opacity = solid ? 1.0f : ALPHA_DEFAULT;
		player.last_opacity_timestamp = 0;
	}

	bool template_init_ = false;
	std::vector<CK_ID> template_balls_;
	IBML* bml_ = nullptr;
	std::function<CK3dObject* ()> get_own_ball_fn_;
	game_state& db_;
	std::recursive_mutex mutex_;
	std::unordered_map<HSteamNetConnection, PlayerObjects> objects_;
	bool extrapolation_ = false, dynamic_opacity_ = true;
	static constexpr SteamNetworkingMicroseconds MAX_EXTRAPOLATION_TIME = 163840;
	static constexpr float MAX_EXTRAPOLATION_SQUARE_DISTANCE = 512.0f;
	static constexpr int64_t MAX_EXTRAPOLATION_TIME_VARIANCE = 268435456ll;
	static constexpr float DILATION_MAX_SQUARE_DISTANCE = 14.0f,
		ALPHA_DEFAULT = 0.5f, ALPHA_DISTANCE_RATE = 0.0144f,
		ALPHA_BEGIN = 0.2f, ALPHA_MIN = 0.28f, ALPHA_MAX = 0.7f;
	const utils* utils_;
};
