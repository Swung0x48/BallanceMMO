#pragma once
#include "sprite_base.h"
#include "bml_includes.h"

class _text_sprite_wrapper : public BGui::Text {
public:
	using BGui::Text::Text;
	void SetBackgroundColor(CKDWORD col) {
		m_sprite->SetBackgroundColor(col);
	}
};

struct text_sprite : public sprite_base<_text_sprite_wrapper> {
	text_sprite() = delete;
	text_sprite(const text_sprite&) = delete;

	text_sprite(const std::string& name,
		const std::string& content,
		float x_pos = 0.0f,
		float y_pos = 0.0f) {
		std::unique_lock lk(mtx_);

		sprite_ = std::make_unique<_text_sprite_wrapper>(("MMO_SpriteText_" + name).c_str());
		sprite_->SetAlignment(CKSPRITETEXT_RIGHT);
		sprite_->SetSize(Vx2DVector(x_pos, 0.1f));
		sprite_->SetPosition(Vx2DVector(0.0f, y_pos)); // Yeah that's right
		sprite_->SetFont("", 10, 500, false, false);
		sprite_->SetText(content.c_str());
		sprite_->SetTextColor(0xFFFFFFFF);
		sprite_->SetVisible(false);
		sprite_->SetZOrder(20);
	}

	bool update(const std::string& text, bool preemptive = true) override {
		if (!preemptive) {
			std::unique_lock lk(mtx_, std::try_to_lock);
			if (lk) {
				if (sprite_) sprite_->SetText(text.c_str());
				return true;
			}
			return false;
		}
		else {
			std::unique_lock lk(mtx_);
			if (sprite_) sprite_->SetText(text.c_str());
		}
		return true;
	}

	void paint(CKDWORD color) {
		std::unique_lock lk(mtx_);
		if (sprite_) sprite_->SetTextColor(color);
	}

	void paint_background(CKDWORD color) {
		std::unique_lock lk(mtx_);
		if (sprite_) sprite_->SetBackgroundColor(color);
	}

	void process() override {} // no-op
};
