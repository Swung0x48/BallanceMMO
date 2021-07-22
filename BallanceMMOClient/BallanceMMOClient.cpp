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
}

void BallanceMMOClient::OnPostStartMenu()
{
    if (!init_) {
        ping_ = std::make_shared<text_sprite>("T_MMO_PING", "Ping: --- ms", RIGHT_MOST, 0.0f, bml_mtx_);
        status_ = std::make_shared<text_sprite>("T_MMO_STATUS", "Disconnected", RIGHT_MOST, 0.025f, bml_mtx_);
        status_->paint(0xffff0000);

        init_ = true;
    }

    m_bml->RegisterCommand(new CommandMMO(client_, props_, bml_mtx_, ping_, status_));
}

void BallanceMMOClient::OnProcess() {
    if (m_bml->GetInputManager()->IsKeyPressed(CKKEY_TAB)) {
        ping_->toggle();
        status_->toggle();
    }

    if (!client_.connected())
        return;

    if (m_bml->IsIngame()) {

    }
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
            status_->update("Connected");
            status_->paint(0xff00ff00);
            std::unique_lock lk(bml_mtx_);
            m_bml->SendIngameMessage("Accepted by server!");
            m_bml->SendIngameMessage((std::string("Welcome back, ") + props_["playername"]->GetString()).c_str());

            m_bml->AddTimerLoop(1000.0f, [this]() {
                ammo::common::message<PacketType> msg;
                uint64_t now = std::chrono::system_clock::now().time_since_epoch().count();
                msg << now;
                msg.header.id = Ping;
                client_.send(msg);
                return client_.connected();
            });
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
            status_->update("Disconnected (Rejected)");
            status_->paint(0xffff0000);
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
                ping_->update(std::format("Ping: {:3} ms", ping / 1000), false);
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
