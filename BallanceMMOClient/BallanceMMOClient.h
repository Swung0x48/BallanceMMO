#pragma once

#include <BML/BMLAll.h>
#include "CommandMMO.h"
#include "text_sprite.h"
#include "label_sprite.h"
#include "client.h"
#include "game_state.h"
#include <unordered_map>
#include <mutex>
#include <memory>
#include <format>
#include <asio.hpp>

extern "C" {
	__declspec(dllexport) IMod* BMLEntry(IBML* bml);
}

class BallanceMMOClient : public IMod, public client {
public:
	BallanceMMOClient(IBML* bml): 
		IMod(bml)
		//client_([this](ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) { LoggingOutput(eType, pszMsg); },
		//	[this](SteamNetConnectionStatusChangedCallback_t* pInfo) { OnConnectionStatusChanged(pInfo); })
	{
		this_instance_ = this;
	}

	virtual CKSTRING GetID() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetVersion() override { return "3.0.0-alpha1"; }
	virtual CKSTRING GetName() override { return "BallanceMMOClient"; }
	virtual CKSTRING GetAuthor() override { return "Swung0x48"; }
	virtual CKSTRING GetDescription() override { return "The client to connect your game to the universe."; }
	DECLARE_BML_VERSION;

	static void init_socket() {
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		SteamDatagramErrMsg err_msg;
		if (!GameNetworkingSockets_Init(nullptr, err_msg))
			FatalError("GameNetworkingSockets_Init failed.  %s", err_msg);
#else
		SteamDatagramClient_SetAppID(570); // Just set something, doesn't matter what
		//SteamDatagramClient_SetUniverse( k_EUniverseDev );

		SteamDatagramErrMsg errMsg;
		if (!SteamDatagramClient_Init(true, errMsg))
			FatalError("SteamDatagramClient_Init failed.  %s", errMsg);

		SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
#endif
		init_timestamp_ = SteamNetworkingUtils()->GetLocalTimestamp();
		SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, LoggingOutput);
	}

private:
	void OnLoad() override;
	void OnPostStartMenu() override;
	void OnExitGame() override;
	void OnUnload() override;
	void OnProcess() override;
	void OnStartLevel() override;
	void OnLoadObject(CKSTRING filename, BOOL isMap, CKSTRING masterName, CK_CLASSID filterClass, BOOL addtoscene, BOOL reuseMeshes, BOOL reuseMaterials, BOOL dynamic, XObjectArray* objArray, CKObject* masterObj) override;
	
	// Custom
	void OnCommand(IBML* bml, const std::vector<std::string>& args);
	void OnTrafo(int from, int to);
	void OnPeerTrafo(uint64_t id, int from, int to);

	// Callbacks from client
	static void LoggingOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg);
	void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) override;
	void on_message(ISteamNetworkingMessage* network_msg) override;

	static void FatalError(const char* fmt, ...) {
		char text[2048];
		va_list ap;
		va_start(ap, fmt);
		vsprintf(text, fmt, ap);
		va_end(ap);
		char* nl = strchr(text, '\0') - 1;
		if (nl >= text && *nl == '\n')
			*nl = '\0';
		LoggingOutput(k_ESteamNetworkingSocketsDebugOutputType_Bug, text);
	}

	std::unordered_map<std::string, IProperty*> props_;
	std::mutex bml_mtx_;
	std::mutex client_mtx_;
	asio::io_context io_ctx_;
	asio::thread_pool thread_pool_;
	std::thread network_thread_;

	const float RIGHT_MOST = 0.98f;

	bool init_ = false;
	//uint64_t id_ = 0;
	std::shared_ptr<text_sprite> ping_;
	std::shared_ptr<text_sprite> status_;

	CK3dObject* player_ball_ = nullptr;
	BallState local_ball_state_;
	std::vector<CK3dObject*> template_balls_;
	std::unordered_map<std::string, uint32_t> ball_name_to_idx_;
	CKDataArray* current_level_array_ = nullptr;

	struct PeerState {
		std::vector<CK3dObject*> balls;
		uint32_t current_ball = 0;
		std::string player_name = "";
		std::unique_ptr<label_sprite> username_label;

		~PeerState() {
			CKDependencies dep;
			dep.Resize(40); dep.Fill(0);
			dep.m_Flags = CK_DEPENDENCIES_CUSTOM;
			dep[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
			dep[CKCID_MESH] = CK_DEPENDENCIES_COPY_MESH_MATERIAL;
			dep[CKCID_3DENTITY] = CK_DEPENDENCIES_COPY_3DENTITY_MESH;
			for (auto* ball : balls) {
				CKDestroyObject(ball, 0, &dep);
			}
		}
	};
	std::mutex peer_mtx_;
	std::unordered_map<uint64_t, PeerState> peer_;
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
			peer_[id].balls[i] = init_spirit_ball(i, id);
		}
	}

	void init_config() {
		GetConfig()->SetCategoryComment("Remote", "Which server to connect to?");
		IProperty* tmp_prop = GetConfig()->GetProperty("Remote", "ServerAddress");
		tmp_prop->SetComment("Remote server address with port. It could be an IP address or a domain name.");
		tmp_prop->SetDefaultString("127.0.0.1:26676");
		props_["remote_addr"] = tmp_prop;
		/*tmp_prop = GetConfig()->GetProperty("Remote", "Port");
		tmp_prop->SetComment("The port that server is running on.");
		tmp_prop->SetDefaultInteger(50000);
		props_["remote_port"] = tmp_prop;*/

		GetConfig()->SetCategoryComment("Player", "Who are you?");
		tmp_prop = GetConfig()->GetProperty("Player", "Playername");
		tmp_prop->SetComment("Your name please?");
		std::srand(std::time(nullptr));
		int random_variable = std::rand() % 1000;
		std::stringstream ss;
		ss << "Player" << std::setw(3) << std::setfill('0') << random_variable;
		tmp_prop->SetDefaultString(ss.str().c_str());
		props_["playername"] = tmp_prop;
	}

	void init_template_balls() {
		CKDataArray* physicalized_ball = m_bml->GetArrayByName("Physicalize_GameBall");

		template_balls_.reserve(physicalized_ball->GetRowCount());
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
			template_balls_.emplace_back(ball);
			ball_name_to_idx_[ball_name] = i; // "Ball_Xxx"
		}
	}

	void poll_and_toggle_debug_info() {
		if (m_bml->GetInputManager()->IsKeyPressed(CKKEY_F3)) {
			ping_->toggle();
			status_->toggle();
		}
	}

	void check_on_trafo(CK3dObject* ball) {
		if (strcmp(ball->GetName(), player_ball_->GetName()) != 0) {
			// OnTrafo
			GetLogger()->Info("OnTrafo, %s -> %s", player_ball_->GetName(), ball->GetName());
			OnTrafo(ball_name_to_idx_[player_ball_->GetName()], ball_name_to_idx_[ball->GetName()]);
			// Update current player ball
			player_ball_ = ball;
			local_ball_state_.type = ball_name_to_idx_[player_ball_->GetName()];
		}
	}

	void update_player_ball_state() {
		player_ball_->GetPosition(&local_ball_state_.position);
		player_ball_->GetQuaternion(&local_ball_state_.rotation);
	}

	void assemble_and_send_state() {
		bmmo::ball_state_msg msg{};
		assert(sizeof(msg.content) == sizeof(local_ball_state_));
		std::memcpy(&(msg.content), &local_ball_state_, sizeof(msg.content));
		send(msg, k_nSteamNetworkingSend_UnreliableNoNagle);
	}

	void cleanup() {
		std::unique_lock<std::mutex> peer_lk(peer_mtx_);
		peer_.clear();
		shutdown();
		io_ctx_.stop();
		if (network_thread_.joinable())
			network_thread_.join();
		thread_pool_.stop();
	}

	void process_username_label() {
		for (auto& peer : peer_) {
			auto& username_label = peer.second.username_label;
			if (username_label != nullptr) {
				username_label->process();
			}
		}
	}

	static std::string pretty_percentage(float value) {
		if (value < 0)
			return "Not available";

		return std::format("{:.2f}%", value * 100.0f);
	}

	static std::string pretty_bytes(float bytes)
	{
		const char* suffixes[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB" };
		int s = 0; // which suffix to use
		while (bytes >= 1024 && s < 7)
		{
			s++;
			bytes /= 1024;
		}

		return std::format("{:.1f}{}", bytes, suffixes[s]);
	}

	static std::string pretty_status(const SteamNetConnectionRealTimeStatus_t& status) {
		std::string s;
		s.reserve(2048);
		s += std::format("Ping: {} ms\n", status.m_nPing);
		s += "ConnectionQualityLocal: " + pretty_percentage(status.m_flConnectionQualityLocal) + "\n";
		s += "ConnectionQualityRemote: " + pretty_percentage(status.m_flConnectionQualityRemote) + "\n";
		s += std::format("Tx: {:.0f}pps, ", status.m_flOutPacketsPerSec) + pretty_bytes(status.m_flOutBytesPerSec) + "/s\n";
		s += std::format("Rx: {:.0f}pps, ", status.m_flInPacketsPerSec) + pretty_bytes(status.m_flInBytesPerSec) + "/s\n";
		s += "Est. MaxBandwidth: " + pretty_bytes(status.m_nSendRateBytesPerSecond) + "/s\n";
		s += std::format("Queue time: {}us\n", status.m_usecQueueTime);
		s += std::format("\nReliable:            \nPending: {}\nUnacked: {}\n", status.m_cbPendingReliable, status.m_cbSentUnackedReliable);
		s += std::format("\nUnreliable:          \nPending: {}\n", status.m_cbPendingUnreliable);
		return s;
	}
};