// robot_eyes.cc
#include "robot_eyes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t kCyan = 0x00E5FF;
constexpr uint32_t kRed = 0xFF3B30;
constexpr uint32_t kBlue = 0x4DA3FF;
constexpr uint32_t kPink = 0xFF5FA2;

constexpr int kTickMs = 33;  // ~30 fps

struct Named {
    const char* name;
    RobotEyes::Pose pose;
};

//                          w     h    radius lidT  angle lidB  gx    gy    color  wink
const Named kTable[] = {
    {"neutral",     {1.00f, 1.00f, 0.35f, 0.00f,   0.f, 0.00f,  0.0f,  0.0f, kCyan, false}},
    {"happy",       {1.00f, 0.85f, 0.50f, 0.00f,   0.f, 0.45f,  0.0f,  0.0f, kCyan, false}},
    {"laughing",    {1.05f, 0.70f, 0.50f, 0.00f,   0.f, 0.60f,  0.0f,  0.0f, kCyan, false}},
    {"funny",       {1.05f, 0.70f, 0.50f, 0.00f,   0.f, 0.60f,  0.0f,  0.0f, kCyan, false}},
    {"silly",       {1.05f, 0.70f, 0.50f, 0.00f,   0.f, 0.60f,  0.0f,  0.0f, kCyan, false}},
    {"sad",         {1.00f, 1.00f, 0.40f, 0.35f, -18.f, 0.00f,  0.0f,  0.1f, kBlue, false}},
    {"crying",      {1.00f, 0.90f, 0.45f, 0.35f, -20.f, 0.00f,  0.0f,  0.2f, kBlue, false}},
    {"angry",       {1.00f, 0.95f, 0.20f, 0.45f,  20.f, 0.00f,  0.0f,  0.0f, kRed,  false}},
    {"loving",      {1.00f, 0.85f, 0.50f, 0.00f,   0.f, 0.40f,  0.0f,  0.0f, kPink, false}},
    {"kissy",       {1.00f, 0.85f, 0.50f, 0.00f,   0.f, 0.40f,  0.0f,  0.0f, kPink, false}},
    {"embarrassed", {0.95f, 0.75f, 0.50f, 0.10f,   0.f, 0.30f,  0.0f,  0.3f, kPink, false}},
    {"surprised",   {0.80f, 1.30f, 0.50f, 0.00f,   0.f, 0.00f,  0.0f,  0.0f, kCyan, false}},
    {"shocked",     {0.70f, 1.40f, 0.50f, 0.00f,   0.f, 0.00f,  0.0f,  0.0f, kCyan, false}},
    {"thinking",    {0.90f, 0.90f, 0.35f, 0.25f,   6.f, 0.00f,  0.6f, -0.6f, kCyan, false}},
    {"confused",    {0.95f, 1.00f, 0.40f, 0.15f,  -6.f, 0.00f, -0.6f,  0.3f, kCyan, false}},
    {"winking",     {1.00f, 1.00f, 0.35f, 0.00f,   0.f, 0.00f,  0.0f,  0.0f, kCyan, true }},
    {"cool",        {1.10f, 0.55f, 0.20f, 0.25f,   0.f, 0.00f,  0.0f,  0.0f, kCyan, false}},
    {"relaxed",     {1.00f, 0.60f, 0.50f, 0.30f,   0.f, 0.20f,  0.0f,  0.0f, kCyan, false}},
    {"delicious",   {1.00f, 0.85f, 0.50f, 0.00f,   0.f, 0.45f,  0.0f,  0.0f, kCyan, false}},
    {"confident",   {1.00f, 0.85f, 0.30f, 0.30f,   8.f, 0.00f,  0.0f,  0.0f, kCyan, false}},
    {"sleepy",      {1.00f, 0.50f, 0.40f, 0.50f,  -8.f, 0.00f,  0.0f,  0.2f, kCyan, false}},
};

inline bool Due(uint32_t now, uint32_t t) {
    return static_cast<int32_t>(now - t) >= 0;
}

// Rango aleatorio con signo en [-1, 1]
inline float RandSigned() {
    return (static_cast<int>(lv_rand(0, 200)) - 100) / 100.0f;
}

lv_obj_t* MakeBlock(lv_obj_t* parent, uint32_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);  // que el touch pase de largo
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    return o;
}

}  // namespace

RobotEyes::RobotEyes(lv_obj_t* parent, int width, int height, uint32_t bg_color)
    : width_(width), height_(height), bg_(bg_color) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(root_, width_, height_);
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_outline_width(root_, 0, 0);
    lv_obj_set_style_shadow_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);

    for (auto& e : eyes_) {
        e.body = MakeBlock(root_, kCyan);
        lv_obj_set_style_clip_corner(e.body, true, 0);  // recorta los párpados con la forma del ojo

        e.lid_top = MakeBlock(e.body, bg_);
        e.lid_bottom = MakeBlock(e.body, bg_);
        lv_obj_set_style_radius(e.lid_bottom, LV_RADIUS_CIRCLE, 0);
    }

    cur_ = target_ = kTable[0].pose;
    color_ = lv_color_hex(cur_.color);

    const uint32_t now = lv_tick_get();
    next_blink_ = now + lv_rand(1500, 3500);
    next_gaze_ = now + lv_rand(1000, 2500);

    Apply();
    timer_ = lv_timer_create(TimerCb, kTickMs, this);
}

RobotEyes::~RobotEyes() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (root_) {
        lv_obj_delete(root_);  // borra también los ojos y párpados (hijos)
        root_ = nullptr;
    }
}

void RobotEyes::SetEmotion(const char* emotion) {
    const Pose* found = &kTable[0].pose;
    if (emotion) {
        for (const auto& n : kTable) {
            if (std::strcmp(n.name, emotion) == 0) {
                found = &n.pose;
                break;
            }
        }
    }
    target_ = *found;

    // Un parpadeo rápido al cambiar de expresión queda más natural.
    blinking_ = true;
    blink_end_ = lv_tick_get() + 110;
}

void RobotEyes::TimerCb(lv_timer_t* t) {
    static_cast<RobotEyes*>(lv_timer_get_user_data(t))->Tick();
}

void RobotEyes::Tick() {
    const uint32_t now = lv_tick_get();

    // --- Parpadeo aleatorio ---
    if (!blinking_ && Due(now, next_blink_)) {
        blinking_ = true;
        blink_end_ = now + 130;
    }
    if (blinking_ && Due(now, blink_end_)) {
        blinking_ = false;
        next_blink_ = now + lv_rand(2000, 5500);
    }

    // --- Mirada errante (idle) ---
    if (Due(now, next_gaze_)) {
        idle_x_ = RandSigned() * 0.18f;
        idle_y_ = RandSigned() * 0.10f;
        next_gaze_ = now + lv_rand(1200, 3500);
    }

    // --- Suavizado hacia la expresión objetivo ---
    constexpr float k = 0.22f;
    auto ease = [](float& c, float t, float kk) { c += (t - c) * kk; };
    ease(cur_.w, target_.w, k);
    ease(cur_.h, target_.h, k);
    ease(cur_.radius, target_.radius, k);
    ease(cur_.lid_top, target_.lid_top, k);
    ease(cur_.lid_angle, target_.lid_angle, k);
    ease(cur_.lid_bottom, target_.lid_bottom, k);
    ease(cur_.gaze_x, target_.gaze_x, k);
    ease(cur_.gaze_y, target_.gaze_y, k);
    ease(blink_k_, blinking_ ? 0.06f : 1.0f, 0.6f);
    ease(wink_k_, target_.wink ? 0.08f : 1.0f, 0.35f);
    color_ = lv_color_mix(lv_color_hex(target_.color), color_, 70);

    Apply();
}

void RobotEyes::Apply() {
    const float base_w = width_ * 0.26f;
    const float base_h = height_ * 0.62f;
    const float gap = width_ * 0.10f;

    const float w = std::max(base_w * cur_.w, 4.0f);
    const float h_open = base_h * cur_.h;

    for (int i = 0; i < 2; ++i) {
        const bool right = (i == 1);
        Eye& e = eyes_[i];

        float k = blink_k_;
        if (right) k = std::min(k, wink_k_);
        const float hh = std::max(h_open * k, 3.0f);

        // Posición: centro del área +/- mitad del ojo y del hueco, más la mirada.
        const float gx = (cur_.gaze_x * 0.12f + idle_x_) * base_w;
        const float gy = (cur_.gaze_y * 0.12f + idle_y_) * base_h;
        const float cx = width_ / 2.0f + (right ? 1.0f : -1.0f) * (w / 2.0f + gap / 2.0f);
        const float x = cx - w / 2.0f + gx;
        const float y = height_ / 2.0f - hh / 2.0f + gy;

        lv_obj_set_size(e.body, lroundf(w), lroundf(hh));
        lv_obj_set_pos(e.body, lroundf(x), lroundf(y));
        lv_obj_set_style_radius(e.body, lroundf(cur_.radius * std::min(w, hh)), 0);
        lv_obj_set_style_bg_color(e.body, color_, 0);

        // Párpado superior inclinado (enfadado / triste / pensativo / cool)
        const bool show_top = cur_.lid_top > 0.01f || std::fabs(cur_.lid_angle) > 0.5f;
        if (show_top) {
            const int W2 = lroundf(w * 1.7f);
            const int H2 = lroundf(hh + w);
            lv_obj_set_size(e.lid_top, W2, H2);
            lv_obj_set_pos(e.lid_top, lroundf(w / 2.0f - W2 / 2.0f), lroundf(cur_.lid_top * hh - H2));
            lv_obj_set_style_transform_pivot_x(e.lid_top, W2 / 2, 0);
            lv_obj_set_style_transform_pivot_y(e.lid_top, H2, 0);
            const float a = right ? -cur_.lid_angle : cur_.lid_angle;  // espejo en el ojo derecho
            lv_obj_set_style_transform_rotation(e.lid_top, lroundf(a * 10.0f), 0);
            lv_obj_remove_flag(e.lid_top, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(e.lid_top, LV_OBJ_FLAG_HIDDEN);
        }

        // Párpado inferior circular (ojos felices en arco)
        if (cur_.lid_bottom > 0.01f) {
            const int D = lroundf(w * 1.5f);
            lv_obj_set_size(e.lid_bottom, D, D);
            lv_obj_set_pos(e.lid_bottom, lroundf(w / 2.0f - D / 2.0f), lroundf(hh * (1.0f - cur_.lid_bottom)));
            lv_obj_remove_flag(e.lid_bottom, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(e.lid_bottom, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
