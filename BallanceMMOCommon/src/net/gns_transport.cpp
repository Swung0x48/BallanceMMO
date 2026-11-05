// GameNetworkingSockets as a bmmo::net::transport (net/gns.hpp, step K2):
// the one file that includes the library.  Every call is the one the hosts
// made before the platform layer, with the same arguments.

#include <net/gns.hpp>

#include <cstring>
#include <vector>

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

namespace bmmo::net::gns {

    // The layer's values are the library's (transport.hpp says why).
    static_assert(sizeof(connection) == sizeof(HSteamNetConnection) && kInvalidConnection == k_HSteamNetConnection_Invalid);
    static_assert(sizeof(microseconds) == sizeof(SteamNetworkingMicroseconds));
    static_assert(kSendUnreliable == k_nSteamNetworkingSend_Unreliable && kSendNoNagle == k_nSteamNetworkingSend_NoNagle
                  && kSendNoDelay == k_nSteamNetworkingSend_NoDelay && kSendReliable == k_nSteamNetworkingSend_Reliable
                  && kSendUnreliableNoNagle == k_nSteamNetworkingSend_UnreliableNoNagle
                  && kSendUnreliableNoDelay == k_nSteamNetworkingSend_UnreliableNoDelay
                  && kSendReliableNoNagle == k_nSteamNetworkingSend_ReliableNoNagle);
    static_assert(static_cast<int>(connection_state::none) == k_ESteamNetworkingConnectionState_None
                  && static_cast<int>(connection_state::connecting) == k_ESteamNetworkingConnectionState_Connecting
                  && static_cast<int>(connection_state::finding_route) == k_ESteamNetworkingConnectionState_FindingRoute
                  && static_cast<int>(connection_state::connected) == k_ESteamNetworkingConnectionState_Connected
                  && static_cast<int>(connection_state::closed_by_peer) == k_ESteamNetworkingConnectionState_ClosedByPeer
                  && static_cast<int>(connection_state::problem_detected_locally)
                      == k_ESteamNetworkingConnectionState_ProblemDetectedLocally);
    static_assert(kEndInvalid == k_ESteamNetConnectionEnd_Invalid && kEndApp == k_ESteamNetConnectionEnd_App_Min
                  && kEndAppMax == k_ESteamNetConnectionEnd_App_Max
                  && kEndAppException == k_ESteamNetConnectionEnd_AppException_Min);
    static_assert(kMaxMessageSize == k_cbMaxSteamNetworkingSocketsMessageSizeSend);
    static_assert(static_cast<int>(bmmo::log_type::bug) == k_ESteamNetworkingSocketsDebugOutputType_Bug
                  && static_cast<int>(bmmo::log_type::msg) == k_ESteamNetworkingSocketsDebugOutputType_Msg
                  && static_cast<int>(bmmo::log_type::everything) == k_ESteamNetworkingSocketsDebugOutputType_Everything);

    namespace {
        log_sink sink_ = nullptr;

        void debug_output(ESteamNetworkingSocketsDebugOutputType type, const char* text) {
            if (sink_) sink_(static_cast<bmmo::log_type>(type), text);
        }

        // The status callback runs inside RunCallbacks, on the polling thread:
        // poll_status points it at its consumer for the call's length.
        thread_local const std::function<void(const status_change&)>* status_consumer_ = nullptr;

        void on_status(SteamNetConnectionStatusChangedCallback_t* info) {
            if (!status_consumer_) return;
            status_change change;
            change.conn = info->m_hConn;
            change.state = static_cast<connection_state>(info->m_info.m_eState);
            change.old_state = static_cast<connection_state>(info->m_eOldState);
            change.end_reason = info->m_info.m_eEndReason;
            change.end_debug = info->m_info.m_szEndDebug;
            change.description = info->m_info.m_szConnectionDescription;
            (*status_consumer_)(change);
        }

        // A listen socket's or a connection's options: the 7.2 s connected
        // timeout (a global the library keeps) and the status callback.
        SteamNetworkingConfigValue_t options() {
            SteamNetworkingConfigValue_t opt{};
            SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_TimeoutConnected, 7200);
            opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)on_status);
            return opt;
        }

        class gns_transport final : public transport {
        public:
            bool listen(uint16_t port) override {
                SteamNetworkingIPAddr local{};
                local.Clear();
                local.m_port = port;
                SteamNetworkingConfigValue_t opt = options();
                listen_socket_ = api_->CreateListenSocketIP(local, 1, &opt);
                if (listen_socket_ == k_HSteamListenSocket_Invalid) return false;
                poll_group_ = api_->CreatePollGroup();
                return poll_group_ != k_HSteamNetPollGroup_Invalid;
            }

            accept_result accept(connection c) override {
                if (api_->AcceptConnection(c) != k_EResultOK) return accept_result::refused;
                if (!api_->SetConnectionPollGroup(c, poll_group_)) return accept_result::no_receive_set;
                return accept_result::ok;
            }

            connection connect(const std::string& address) override {
                SteamNetworkingIPAddr remote{};
                if (!remote.ParseString(address.c_str())) return kInvalidConnection;
                SteamNetworkingConfigValue_t opt = options();
                connection_ = api_->ConnectByIPAddress(remote, 1, &opt);
                return connection_;
            }

            bool send(connection c, const void* data, size_t size, int flags) override {
                return api_->SendMessageToConnection(c, data, static_cast<uint32>(size), flags, nullptr) == k_EResultOK;
            }

            void flush(connection c) override { api_->FlushMessagesOnConnection(c); }

            void close(connection c, int reason, const std::string& debug, bool linger) override {
                api_->CloseConnection(c, reason, debug.empty() ? nullptr : debug.c_str(), linger);
            }

            void set_name(connection c, const std::string& name) override { api_->SetConnectionName(c, name.c_str()); }

            bool status(connection c, connection_status& out) override {
                SteamNetConnectionRealTimeStatus_t s{};
                const bool ok = api_->GetConnectionRealTimeStatus(c, &s, 0, nullptr) == k_EResultOK;
                out.ping_ms = s.m_nPing;
                out.quality_local = s.m_flConnectionQualityLocal;
                out.quality_remote = s.m_flConnectionQualityRemote;
                out.out_packets_per_sec = s.m_flOutPacketsPerSec;
                out.out_bytes_per_sec = s.m_flOutBytesPerSec;
                out.in_packets_per_sec = s.m_flInPacketsPerSec;
                out.in_bytes_per_sec = s.m_flInBytesPerSec;
                out.send_rate_bytes_per_sec = s.m_nSendRateBytesPerSecond;
                out.pending_unreliable = s.m_cbPendingUnreliable;
                out.pending_reliable = s.m_cbPendingReliable;
                out.sent_unacked_reliable = s.m_cbSentUnackedReliable;
                out.queue_time = s.m_usecQueueTime;
                return ok;
            }

            std::string detailed_status(connection c) override {
                char text[2048] = {};
                api_->GetDetailedConnectionStatus(c, text, sizeof(text));
                return text;
            }

            std::string remote_address(connection c) override {
                SteamNetConnectionInfo_t info{};
                if (!api_->GetConnectionInfo(c, &info)) return {};
                char text[SteamNetworkingIPAddr::k_cchMaxString] = {};
                info.m_addrRemote.ToString(text, sizeof(text), false);
                return text;
            }

            int receive(const std::function<void(const message&)>& each, int max) override {
                if (static_cast<int>(incoming_.size()) < max) incoming_.resize(static_cast<size_t>(max));
                const int count = poll_group_ != k_HSteamNetPollGroup_Invalid
                    ? api_->ReceiveMessagesOnPollGroup(poll_group_, incoming_.data(), max)
                    : api_->ReceiveMessagesOnConnection(connection_, incoming_.data(), max);
                for (int i = 0; i < count; ++i) {
                    ISteamNetworkingMessage* m = incoming_[static_cast<size_t>(i)];
                    each(message{m->m_conn, m->m_pData, m->m_cbSize, m->m_usecTimeReceived});
                    m->Release();
                }
                return count;
            }

            void poll_status(const std::function<void(const status_change&)>& each) override {
                const auto* outer = status_consumer_;
                status_consumer_ = &each;
                api_->RunCallbacks();
                status_consumer_ = outer;
            }

            microseconds local_timestamp() override { return process_timestamp(); }

            bool set_fake_lag(direction d, int ms) override {
                return SteamNetworkingUtils()->SetGlobalConfigValueInt32(
                    d == direction::send ? k_ESteamNetworkingConfig_FakePacketLag_Send : k_ESteamNetworkingConfig_FakePacketLag_Recv,
                    ms);
            }

            bool set_fake_loss(direction d, float percent) override {
                return SteamNetworkingUtils()->SetGlobalConfigValueFloat(
                    d == direction::send ? k_ESteamNetworkingConfig_FakePacketLoss_Send : k_ESteamNetworkingConfig_FakePacketLoss_Recv,
                    percent);
            }

            int connection_timeout_ms() override {
                int32_t timeout = 0;
                size_t size = sizeof(timeout);
                ESteamNetworkingConfigDataType type{};
                if (SteamNetworkingUtils()->GetConfigValue(k_ESteamNetworkingConfig_TimeoutConnected,
                                                           k_ESteamNetworkingConfig_Global, 0, &type, &timeout, &size)
                        != k_ESteamNetworkingGetConfigValue_OK)
                    return 0;
                return timeout;
            }

        private:
            ISteamNetworkingSockets* api_ = SteamNetworkingSockets();
            HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
            HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
            connection connection_ = kInvalidConnection;   // a client's (receive reads it)
            std::vector<ISteamNetworkingMessage*> incoming_;
        };
    }

    bool init(log_sink sink, std::string& error) {
        SteamDatagramErrMsg message{};
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
        if (!GameNetworkingSockets_Init(nullptr, message)) {
            error = message;
            return false;
        }
#else
        SteamDatagramClient_SetAppID(570); // Just set something, doesn't matter what
        if (!SteamDatagramClient_Init(true, message)) {
            error = message;
            return false;
        }
        // Authentication is disabled automatically in the open-source build.
        SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
#endif
        sink_ = sink;
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, debug_output);
        return true;
    }

    void kill() {
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
        GameNetworkingSockets_Kill();
#else
        SteamDatagramClient_Kill();
#endif
    }

    void set_log_level(bmmo::log_type least_serious) {
        SteamNetworkingUtils()->SetDebugOutputFunction(static_cast<ESteamNetworkingSocketsDebugOutputType>(least_serious),
                                                       debug_output);
    }

    std::unique_ptr<transport> make_transport() { return std::make_unique<gns_transport>(); }

}

namespace bmmo::net {
    microseconds process_timestamp() { return SteamNetworkingUtils()->GetLocalTimestamp(); }
}
