#include "BallanceMMOClient.h"

IMod* BMLEntry(IBML* bml) {
	return new BallanceMMOClient(bml);
}

void BallanceMMOClient::OnPostStartMenu() {
	client_ = new Client();
	client_->connect("192.168.50.100", 60000);
	if (client_->is_connected())
		m_bml->SendIngameMessage("Connected!");
}



void BallanceMMOClient::OnProcess()
{
	if (m_bml->IsPlaying()) {
		//VxVector pos;
		//VxQuaternion rot;
		//ball->GetPosition(&pos);
		//ball->GetQuaternion(&rot);
		
		//msg_.clear();
		//msg_.header.id = MsgType::MessageAll;
		//msg_ << pos << rot;
		//client_->broadcast_message(msg_);
		//client_->ping_server();
		VxVector pos;
		VxQuaternion rot;
		player_ball_->GetPosition(&pos);
		player_ball_->GetQuaternion(&rot);
		if (pos == position_ && rot == rotation_)
			return;
		msg_.clear();
		msg_.header.id = MsgType::MessageAll;
		msg_ << pos << rot;
		client_->broadcast_message(msg_);
	}
}

void BallanceMMOClient::OnStartLevel()
{
	player_ball_ = static_cast<CK3dObject*>(m_bml->GetArrayByName("CurrentLevel")->GetElementObject(0, 1));
	using namespace std::chrono_literals;
	
}

