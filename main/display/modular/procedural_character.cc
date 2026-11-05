#include "character.h"
#include "ui_style.h"

#include <cstring>
#include <new>

namespace {

struct Look {
    int eye_w, eye_h, eye_dx, eye_dy;
    int mouth_w, mouth_h, mouth_dy;
    bool ring;
    bool talk;
    bool zzz;
    bool dim;
};

const Look& LookFor(CharacterState s) {
    static const Look kIdle{16, 22, 38, -18, 34, 6, 30, false, false, false, false};
    static const Look kListening{20, 28, 38, -20, 18, 18, 32, true, false, false, false};
    static const Look kThinking{16, 22, 38, -30, 22, 5, 32, false, false, false, false};
    static const Look kSpeaking{16, 22, 38, -18, 36, 14, 30, false, true, false, false};
    static const Look kJoy{24, 8, 38, -18, 58, 26, 24, false, false, false, false};
    static const Look kSurprise{24, 30, 38, -22, 22, 28, 30, false, false, false, false};
    static const Look kSad{16, 14, 38, -10, 34, 6, 42, false, false, false, false};
    static const Look kSleep{28, 4, 38, -18, 20, 5, 30, false, false, true, false};
    static const Look kOffline{16, 22, 38, -18, 30, 4, 40, false, false, false, true};
    switch (s) {
        case CharacterState::kListening:
            return kListening;
        case CharacterState::kThinking:
        case CharacterState::kThoughtful:
            return kThinking;
        case CharacterState::kSpeaking:
            return kSpeaking;
        case CharacterState::kJoy:
        case CharacterState::kGreeting:
        case CharacterState::kDone:
            return kJoy;
        case CharacterState::kSurprise:
            return kSurprise;
        case CharacterState::kSad:
            return kSad;
        case CharacterState::kSleep:
            return kSleep;
        case CharacterState::kOffline:
            return kOffline;
        default:
            return kIdle;
    }
}

constexpr uint32_t kAccent = ModularStyle::kAccent;
constexpr uint32_t kBody = 0xF2F2F2;
constexpr uint32_t kBodyDim = 0x7A7A7A;

void AnimSetHeight(void* obj, int32_t v) { lv_obj_set_height(static_cast<lv_obj_t*>(obj), v); }
void AnimSetY(void* obj, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(obj), v); }
void AnimSetBorderOpa(void* obj, int32_t v) {
    lv_obj_set_style_border_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}

lv_obj_t* MakeShape(lv_obj_t* parent, int w, int h, uint32_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

class ProceduralCharacter : public Character {
public:
    ~ProceduralCharacter() override {
        if (body_ != nullptr) {
            lv_anim_delete(body_, AnimSetY);
            lv_anim_delete(mouth_, AnimSetHeight);
        }
        if (ring_ != nullptr) {
            lv_anim_delete(ring_, AnimSetBorderOpa);
            lv_obj_delete(ring_);
        }
        if (body_ != nullptr) {
            lv_obj_delete(body_);
        }
        if (zzz_ != nullptr) {
            lv_obj_delete(zzz_);
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

        body_ = MakeShape(parent, 190, 220, kBody);
        lv_obj_align(body_, LV_ALIGN_CENTER, 0, 4);

        eye_l_ = MakeShape(body_, 16, 22, 0x101010);
        eye_r_ = MakeShape(body_, 16, 22, 0x101010);
        mouth_ = MakeShape(body_, 34, 6, 0x101010);

        zzz_ = lv_label_create(parent);
        lv_label_set_text(zzz_, "z z z");
        lv_obj_set_style_text_color(zzz_, lv_color_hex(kAccent), 0);
        lv_obj_align(zzz_, LV_ALIGN_CENTER, 90, -100);
        lv_obj_add_flag(zzz_, LV_OBJ_FLAG_HIDDEN);

        StartBreathing();
        SetState(CharacterState::kIdle);
    }

    void SetState(CharacterState state) override {
        state_ = state;
        if (!active_)
            return;
        if (body_ == nullptr) {
            return;
        }
        const Look& l = LookFor(state);

        lv_obj_set_size(eye_l_, l.eye_w, l.eye_h);
        lv_obj_set_size(eye_r_, l.eye_w, l.eye_h);
        lv_obj_align(eye_l_, LV_ALIGN_CENTER, -l.eye_dx, l.eye_dy);
        lv_obj_align(eye_r_, LV_ALIGN_CENTER, l.eye_dx, l.eye_dy);
        lv_obj_set_size(mouth_, l.mouth_w, l.mouth_h);
        lv_obj_align(mouth_, LV_ALIGN_CENTER, 0, l.mouth_dy);
        lv_obj_set_style_bg_color(body_, lv_color_hex(l.dim ? kBodyDim : kBody), 0);

        lv_anim_delete(mouth_, AnimSetHeight);
        if (l.talk) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, mouth_);
            lv_anim_set_exec_cb(&a, AnimSetHeight);
            lv_anim_set_values(&a, 6, 26);
            lv_anim_set_duration(&a, 220);
            lv_anim_set_reverse_duration(&a, 220);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }

        lv_anim_delete(ring_, AnimSetBorderOpa);
        if (l.ring) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, ring_);
            lv_anim_set_exec_cb(&a, AnimSetBorderOpa);
            lv_anim_set_values(&a, 40, 255);
            lv_anim_set_duration(&a, 700);
            lv_anim_set_reverse_duration(&a, 700);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        } else {
            lv_obj_set_style_border_opa(ring_, LV_OPA_TRANSP, 0);
        }

        if (l.zzz) {
            lv_obj_remove_flag(zzz_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(zzz_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void SetActive(bool active) override {
        if (active == active_)
            return;
        active_ = active;
        if (active) {
            StartBreathing();
            SetState(state_);
        } else {
            lv_anim_delete(body_, AnimSetY);
            lv_anim_delete(mouth_, AnimSetHeight);
            lv_anim_delete(ring_, AnimSetBorderOpa);
            lv_obj_set_style_border_opa(ring_, LV_OPA_TRANSP, 0);
        }
    }

private:
    void StartBreathing() {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, body_);
        lv_anim_set_exec_cb(&a, AnimSetY);
        lv_anim_set_values(&a, 0, 8);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_duration(&a, 2400);
        lv_anim_set_reverse_duration(&a, 2400);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }

    lv_obj_t* ring_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* eye_l_ = nullptr;
    lv_obj_t* eye_r_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* zzz_ = nullptr;
    bool active_ = true;
    CharacterState state_ = CharacterState::kIdle;
};

bool Is(const char* e, const char* const* list) {
    for (; *list; ++list) {
        if (strcmp(e, *list) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

CharacterState CharacterStateFromEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return CharacterState::kIdle;
    }
    static const char* const joy[] = {"happy",     "laughing",  "funny",   "loving",
                                      "kissy",     "delicious", "winking", "cool",
                                      "confident", "silly",     nullptr};
    static const char* const sad[] = {"sad", "crying", "angry", "embarrassed", nullptr};
    static const char* const surprise[] = {"surprised", "shocked", nullptr};
    static const char* const thinking[] = {"thinking", "confused", nullptr};
    static const char* const sleep[] = {"sleepy", "relaxed", nullptr};
    if (Is(emotion, joy))
        return CharacterState::kJoy;
    if (Is(emotion, sad))
        return CharacterState::kSad;
    if (Is(emotion, surprise))
        return CharacterState::kSurprise;
    if (Is(emotion, thinking))
        return CharacterState::kThoughtful;
    if (Is(emotion, sleep))
        return CharacterState::kSleep;
    return CharacterState::kIdle;
}

Character* CreateProceduralCharacter() { return new (std::nothrow) ProceduralCharacter(); }
