#pragma once
#include "sprite_base.h"
#include "bml_includes.h"

struct label_sprite : public sprite_base<BGui::Label> {
    label_sprite() = delete;
    label_sprite(const label_sprite&) = delete;

    label_sprite(const std::string& name,
        const std::string& content,
        float x_pos,
        float y_pos) {
        std::unique_lock lk(mtx_);

        sprite_ = std::make_unique<BGui::Label>(("MMO_SpriteLabel_" + name).c_str());
        sprite_->SetAlignment(ALIGN_CENTER);
        sprite_->SetSize(Vx2DVector(0.06f, 0.03f));
        sprite_->SetPosition(Vx2DVector(x_pos, y_pos));
        sprite_->SetText(content.c_str());
        sprite_->SetFont(ExecuteBB::GAMEFONT_03);
        sprite_->SetVisible(false);
        sprite_->SetZOrder(20);
    }

    bool update(const std::string& text, [[maybe_unused]] bool preemptive = true) override {
        std::unique_lock lk(mtx_);
        if (sprite_) sprite_->SetText(text.c_str());
        return true;
    }

    void process() override {
        std::unique_lock lk(mtx_);
        if (sprite_) sprite_->Process();
    }
};

