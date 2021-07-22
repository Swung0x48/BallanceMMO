#pragma once
#include <memory>
#include <mutex>
#include <BML/BMLAll.h>
struct text_sprite {
	std::unique_ptr<BGui::Text> sprite_;
	std::mutex mtx_;
	std::mutex& bml_mtx_;
	bool visible_ = true;

	text_sprite() = delete;
	text_sprite(const text_sprite&) = delete;

	text_sprite(const std::string& name,
		const std::string& content,
		float x_pos,
		float y_pos,
		std::mutex& bml_mtx) :
		bml_mtx_(bml_mtx) {
		std::unique_lock bml_lk(bml_mtx_);
		std::unique_lock lk(mtx_);

		sprite_ = std::make_unique<BGui::Text>(name.c_str());
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
		std::unique_lock bml_lk(bml_mtx_);
		if (!preemptive) {
			std::unique_lock lk(mtx_, std::try_to_lock);
			if (lk) {
				sprite_.get()->SetText(text.c_str());
				return true;
			}
			return false;
		}
		else {
			std::unique_lock lk(mtx_);
			sprite_.get()->SetText(text.c_str());
		}
		return true;
	}
	void paint(CKDWORD color) {
		std::unique_lock bml_lk(bml_mtx_);
		std::unique_lock lk(mtx_);
		sprite_->SetTextColor(color);
	}
	void toggle() {
		std::unique_lock bml_lk(bml_mtx_);
		std::unique_lock lk(mtx_);
		visible_ = !visible_;
		sprite_->SetVisible(visible_);
	}
};
