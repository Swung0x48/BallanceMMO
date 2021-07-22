#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
	return new BallanceMMOClient(bml);
}

void BallanceMMOClient::OnLoad()
{
	GetConfig()->SetCategoryComment("Remote", "Which server to connect to?");
	IProperty* tmp_prop = GetConfig()->GetProperty("Remote", "ServerAddress");
	tmp_prop->SetComment("Remote server address, it could be an IP address or a domain name.");
	tmp_prop->SetDefaultString("127.0.0.1");
	props_["remote_addr"] = tmp_prop;
	tmp_prop = GetConfig()->GetProperty("Remote", "Port");
	tmp_prop->SetComment("The port that server is running on.");
	tmp_prop->SetDefaultInteger(50000);
	props_["remote_port"] = tmp_prop;

	GetConfig()->SetCategoryComment("Player", "Who are you?");
	tmp_prop = GetConfig()->GetProperty("Player", "Playername");
	tmp_prop->SetComment("Your name please?");
	std::srand(std::time(nullptr));
	int random_variable = std::rand() % 1000;
	std::stringstream ss;
	ss << "Player" << std::setw(3) << std::setfill('0') << random_variable;
	tmp_prop->SetDefaultString(ss.str().c_str());
	props_["playername"] = tmp_prop;

	m_bml->RegisterCommand(new CommandMMO(client_, props_, bml_mtx_));
}

void BallanceMMOClient::OnExitGame()
{
	try {
		client_.disconnect();
		client_.shutdown();
	} catch (std::exception& e) {
		GetLogger()->Error(e.what());
	}
}

void BallanceMMOClient::OnUnload() {
	
}

void BallanceMMOClient::OnMessage(ammo::common::owned_message<PacketType>& msg)
{
    if (!client_.connected()) {
        if (msg.message.header.id == ConnectionAccepted) {
            client_.confirm_validation();
            std::scoped_lock lk(bml_mtx_);
            m_bml->SendIngameMessage("Accepted by server!");
            m_bml->SendIngameMessage((std::string("Welcome back, ") + props_["playername"]->GetString()).c_str());
        }
        else if (msg.message.header.id == ConnectionChallenge) {
            uint64_t checksum;
            msg.message >> checksum;
            checksum = encode_for_validation(checksum);
            msg.message.clear();
            msg.message << checksum;
            ammo::entity::string<PacketType> str = props_["playername"]->GetString();
            str.serialize(msg.message);
            msg.message.header.id = ConnectionResponse;
            client_.send(msg.message);
        }
        else if (msg.message.header.id == Denied) {
            std::scoped_lock lk(bml_mtx_);
            m_bml->SendIngameMessage("Rejected by server.");
        }
    }
    else {
        switch (msg.message.header.id) {
            case PacketFragment: {
                break;
            }
            case Denied: {
                break;
            }
            case Ping: {
                auto now = std::chrono::system_clock::now().time_since_epoch().count();
                uint64_t then; msg.message >> then;
                auto ping = now - then; // in microseconds
                std::cout << "[INFO] Ping: " << ping / 1000 << " ms" << std::endl;
                break;
            }
            case GameState: {
                break;
            }
            default: {
                GetLogger()->Warn("Unknown message ID: %d", msg.message.header.id);
            }
        }
    }
}
