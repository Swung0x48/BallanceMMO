#pragma once
#include <memory>
#include <mutex>
#include <BML/BMLAll.h>
struct text_sprite {
	std::unique_ptr<BGui::Text> sprite_;
	std::mutex mtx_;
	bool visible_ = true;

	text_sprite() = delete;
	text_sprite(const text_sprite&) = delete;

	text_sprite(const std::string& name,
		const std::string& content,
		float x_pos = 0.0f,
		float y_pos = 0.0f) {
		std::unique_lock lk(mtx_);

		sprite_ = std::make_unique<BGui::Text>(("T_Sprite_" + name).c_str());
		sprite_->SetAlignment(CKSPRITETEXT_RIGHT);
		sprite_->SetSize(Vx2DVector(x_pos, 0.1f));
		sprite_->SetPosition(Vx2DVector(0.0f, y_pos)); // Yeah that's right
		sprite_->SetFont("", 10, 500, false, false);
		sprite_->SetText(content.c_str());
		sprite_->SetTextColor(0xFFFFFFFF);
		sprite_->SetVisible(false);
		sprite_->SetZOrder(20);
	};
	bool update(const std::string& text, bool preemptive = true) {
		if (!preemptive) {
			std::unique_lock lk(mtx_, std::try_to_lock);
			if (lk) {
				sprite_->SetText(text.c_str());
				return true;
			}
			return false;
		}
		else {
			std::unique_lock lk(mtx_);
			sprite_->SetText(text.c_str());
		}
		return true;
	}
	void paint(CKDWORD color) {
		std::unique_lock lk(mtx_);
		sprite_->SetTextColor(color);
	}
	void toggle() {
		std::unique_lock lk(mtx_);
		visible_ = !visible_;
		sprite_->SetVisible(visible_);
	}
};
