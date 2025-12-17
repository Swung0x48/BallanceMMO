#pragma once
#include <memory>
#include <mutex>
#include "bml_includes.h"

struct sprite_interface {
    bool visible_ = false;
    virtual ~sprite_interface() = default;
    virtual void process() = 0;
    virtual void set_visible(bool visible) = 0;
    virtual void toggle() = 0;
    virtual void set_position(const Vx2DVector& position) = 0;
    virtual bool update(const std::string& text, bool preemptive = true) = 0;
};

template<typename T>
struct sprite_base : public sprite_interface {
    std::unique_ptr<T> sprite_;
    std::mutex mtx_;

    sprite_base() = default;
    sprite_base(const sprite_base&) = delete;

    // Common operations shared by all sprites
    void set_position(const Vx2DVector& position) override {
        std::unique_lock lk(mtx_);
        if (sprite_) sprite_->SetPosition(position);
    }

    void set_visible(bool visible) override {
        std::unique_lock lk(mtx_);
        if (visible_ == visible)
            return;
        visible_ = visible;
        if (sprite_) sprite_->SetVisible(visible_);
    }

    void toggle() override {
        set_visible(!visible_);
    }

    T* get() {
        std::unique_lock lk(mtx_);
        return sprite_.get();
    }
};