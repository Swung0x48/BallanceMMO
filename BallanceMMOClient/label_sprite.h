#pragma once
#include <memory>
#include <mutex>
#include <BML/BMLAll.h>
struct label_sprite {
	std::unique_ptr<BGui::Label> sprite_;
	std::mutex mtx_;
	bool visible_ = true;

	label_sprite() = delete;
	label_sprite(const label_sprite&) = delete;

	label_sprite(const std::string& name,
		const std::string& content,
		float x_pos,
		float y_pos) {
		std::unique_lock lk(mtx_);

		sprite_ = std::make_unique<BGui::Label>(("MMO_Name_" + name).c_str());
		sprite_->SetAlignment(ALIGN_CENTER);
		sprite_->SetSize(Vx2DVector(0.1f, 0.03f));
		sprite_->SetPosition(Vx2DVector(x_pos, y_pos));
		sprite_->SetText(content.c_str());
		sprite_->SetFont(ExecuteBB::GAMEFONT_03);
		sprite_->SetVisible(false);
		sprite_->SetZOrder(20);
	};
	void update(const std::string& text) {
		std::unique_lock lk(mtx_);
		sprite_->SetText(text.c_str());
	}

	void toggle() {
		std::unique_lock lk(mtx_);
		visible_ = !visible_;
		sprite_->SetVisible(visible_);
	}

	void set_position(Vx2DVector pos) {
		std::unique_lock lk(mtx_);
		sprite_->SetPosition(pos);
	}
};

