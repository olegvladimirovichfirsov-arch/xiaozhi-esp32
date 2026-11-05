#include <cstring>
#include <new>

#include "character.h"
#include "ui_style.h"

extern "C" {
extern const lv_image_dsc_t ghost_cat_joy, ghost_cat_surprise, ghost_cat_thoughtful, ghost_cat_sad,
    ghost_cat_sleep, ghost_cat_greeting;
extern const lv_image_dsc_t gradient_joy, gradient_surprise, gradient_thoughtful, gradient_sad,
    gradient_sleep, gradient_greeting;
}

namespace {

struct PackImages {
    const char* id;
    const lv_image_dsc_t* joy;
    const lv_image_dsc_t* surprise;
    const lv_image_dsc_t* thoughtful;
    const lv_image_dsc_t* sad;
    const lv_image_dsc_t* sleep;
    const lv_image_dsc_t* greeting;
};

const PackImages kPacks[] = {
    {"ghost_cat", &ghost_cat_joy, &ghost_cat_surprise, &ghost_cat_thoughtful, &ghost_cat_sad,
     &ghost_cat_sleep, &ghost_cat_greeting},
    {"gradient", &gradient_joy, &gradient_surprise, &gradient_thoughtful, &gradient_sad,
     &gradient_sleep, &gradient_greeting},
};

constexpr uint32_t kAccent = ModularStyle::kAccent;

void AnimSetBorderOpa(void* obj, int32_t v) {
    lv_obj_set_style_border_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}

class ImageCharacter : public Character {
public:
    explicit ImageCharacter(const PackImages* pack) : pack_(pack) {}

    ~ImageCharacter() override {
        if (talk_timer_ != nullptr) {
            lv_timer_delete(talk_timer_);
        }
        if (ring_ != nullptr) {
            lv_anim_delete(ring_, AnimSetBorderOpa);
            lv_obj_delete(ring_);
        }
        if (image_ != nullptr) {
            lv_obj_delete(image_);
        }
    }

    void Attach(lv_obj_t* parent) override {
        ring_ = lv_obj_create(parent);
        lv_obj_remove_flag(ring_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(ring_, 236, 236);
        lv_obj_set_style_radius(ring_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring_, 4, 0);
        lv_obj_set_style_border_color(ring_, lv_color_hex(kAccent), 0);
        lv_obj_set_style_border_opa(ring_, LV_OPA_TRANSP, 0);
        lv_obj_align(ring_, LV_ALIGN_CENTER, 0, 4);

        image_ = lv_image_create(parent);
        lv_obj_align(image_, LV_ALIGN_CENTER, 0, 4);

        talk_timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<ImageCharacter*>(lv_timer_get_user_data(t));
                self->talk_phase_ = !self->talk_phase_;
                self->Show(self->talk_phase_ ? self->pack_->greeting : self->pack_->joy);
            },
            320, this);
        if (talk_timer_)
            lv_timer_pause(talk_timer_);

        SetState(CharacterState::kIdle);
    }

    void SetState(CharacterState state) override {
        state_ = state;
        if (!active_)
            return;
        if (image_ == nullptr) {
            return;
        }
        if (talk_timer_)
            lv_timer_pause(talk_timer_);
        lv_anim_delete(ring_, AnimSetBorderOpa);
        lv_obj_set_style_border_opa(ring_, LV_OPA_TRANSP, 0);

        switch (state) {
            case CharacterState::kListening:
                Show(pack_->surprise);
                StartRing();
                break;
            case CharacterState::kSpeaking:
                Show(pack_->joy);
                talk_phase_ = false;
                if (talk_timer_)
                    lv_timer_resume(talk_timer_);
                break;
            case CharacterState::kJoy:
            case CharacterState::kDone:
                Show(pack_->joy);
                break;
            case CharacterState::kSurprise:
                Show(pack_->surprise);
                break;
            case CharacterState::kSad:
            case CharacterState::kOffline:
                Show(pack_->sad);
                break;
            case CharacterState::kSleep:
                Show(pack_->sleep);
                break;
            case CharacterState::kGreeting:
                Show(pack_->greeting);
                break;
            case CharacterState::kThinking:
            case CharacterState::kThoughtful:
            case CharacterState::kIdle:
            default:
                Show(pack_->thoughtful);
                break;
        }
    }

    void SetActive(bool active) override {
        if (active == active_)
            return;
        active_ = active;
        if (active)
            SetState(state_);
        else {
            if (talk_timer_)
                lv_timer_pause(talk_timer_);
            lv_anim_delete(ring_, AnimSetBorderOpa);
            lv_obj_set_style_border_opa(ring_, LV_OPA_TRANSP, 0);
        }
    }

private:
    void Show(const lv_image_dsc_t* dsc) {
        if (dsc != current_) {
            current_ = dsc;
            lv_image_set_src(image_, dsc);
        }
    }

    void StartRing() {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ring_);
        lv_anim_set_exec_cb(&a, AnimSetBorderOpa);
        lv_anim_set_values(&a, 40, 255);
        lv_anim_set_duration(&a, 700);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_reverse_duration(&a, 700);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }

    const PackImages* pack_;
    lv_obj_t* ring_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_timer_t* talk_timer_ = nullptr;
    const lv_image_dsc_t* current_ = nullptr;
    bool talk_phase_ = false;
    bool active_ = true;
    CharacterState state_ = CharacterState::kIdle;
};

}  // namespace

Character* CreateCharacterById(const char* id) {
    if (id != nullptr) {
        for (const auto& p : kPacks) {
            if (strcmp(p.id, id) == 0) {
                return new (std::nothrow) ImageCharacter(&p);
            }
        }
    }
    return CreateProceduralCharacter();
}

int CharacterPackCount() { return sizeof(kPacks) / sizeof(kPacks[0]); }

const char* CharacterPackId(int index) {
    return (index >= 0 && index < CharacterPackCount()) ? kPacks[index].id : nullptr;
}
