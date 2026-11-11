// robot_eyes.cc  (versión 3: tacto + inclinación + sacudida)
#include "robot_eyes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

const char* TAG = "RobotEyes";

constexpr uint32_t kCyan = 0x00E5FF;
constexpr uint32_t kRed = 0xFF3B30;
constexpr uint32_t kBlue = 0x4DA3FF;
constexpr uint32_t kPink = 0xFF5FA2;
constexpr uint32_t kAmber = 0xFFD23C;

constexpr int kTickMs = 33;  // ~30 fps

// ---- Ajustes del tacto (puedes cambiarlos a tu gusto) ----
constexpr uint32_t kTapMaxMs = 500;       // un toque más corto que esto es un "toque"
constexpr uint32_t kDoubleTapMs = 450;    // segundo toque dentro de este tiempo = doble toque
constexpr uint32_t kLongPressMs = 700;    // mantener esto = caricia
constexpr uint32_t kSleepPressMs = 3000;  // mantener esto = sueño
constexpr int kMovePx = 25;               // si el dedo se mueve más, ya no es toque ni pulsación
constexpr uint32_t kTapReactMs = 900;     // duración de la sorpresa
constexpr uint32_t kDoubleReactMs = 1600; // duración de la risa
constexpr uint32_t kReleaseHoldMs = 500;  // tras soltar una caricia/sueño, tarda esto en volver
constexpr float kTouchGazeX = 0.30f;      // cuánto se desplazan los ojos al seguir el dedo
constexpr float kTouchGazeY = 0.25f;

// ---- Ajustes del sensor de movimiento ----
// Si al inclinar a la DERECHA los ojos miran a la IZQUIERDA, cambia 1.0f por -1.0f.
constexpr float kTiltSign = 1.0f;
// Si inclinar a los lados no hace nada pero inclinar hacia delante/atrás sí, pon true.
constexpr bool kTiltUseY = false;
constexpr float kTiltFullG = 0.5f;        // inclinación (en g) con la que los ojos miran al máximo
constexpr float kTiltDeadG = 0.08f;       // por debajo de esto se considera "recto"
constexpr float kTiltGazeX = 0.30f;       // cuánto se desplazan los ojos al inclinar
constexpr float kShakeThresholdG = 0.8f;  // fuerza mínima de un "golpe" de la sacudida
constexpr int kShakeSpikes = 4;           // golpes necesarios...
constexpr uint32_t kShakeWindowMs = 900;  // ...dentro de este tiempo
constexpr uint32_t kShakeCooldownMs = 3000;  // pausa mínima entre sacudidas detectadas
constexpr uint32_t kShakeDurationMs = 2500;  // cuánto dura el mareo

struct Named {
    const char* name;
    RobotEyes::Pose pose;
};

//                          w     h    radius lidT  angle lidB  gx    gy    color  wink
const Named kTable[] = {
    {"neutral",     {1.00f, 1.00f, 0.35f, 0.00f,   0.f, 0.00f,  0.0f,  0.0f, kCyan,  false}},
    {"happy",       {1.00f, 0.85f, 0.50f, 0.00f,   0.f, 0.45f,  0.0f,  0.0f, kCyan,  false}},
    {"laughing",    {1.05f, 0.70f, 0.50f, 0.00f,   0.f, 0.60f,  0.0f,  0.0f, kCyan,  false}},
    {"funny",       {1.05f, 0.70f, 0.50f, 0.00f,   0.f, 0.60f,  0.0f,  0.0f, kCyan,  false}},
    {"silly",       {1.05f, 0.70f, 0.50f, 0.00f,   0.f, 0.60f,  0.0f,  0.0f, kCyan,  false}},
    {"sad",         {1.00f, 1.00f, 0.40f, 0.35f, -18.f, 0.00f,  0.0f,  0.1f, kBlue,  false}},
    {"crying",      {1.00f, 0.90f, 0.45f, 0.35f, -20.f, 0.00f,  0.0f,  0.2f, kBlue,  false}},
    {"angry",       {1.00f, 0.95f, 0.20f, 0.45f,  20.f, 0.00f,  0.0f,  0.0f, kRed,   false}},
    {"loving",      {1.00f, 0.85f, 0.50f, 0.00f,   0.f, 0.40f,  0.0f,  0.0f, kPink,  false}},
    {"kissy",       {1.00f, 0.85f, 0.50f, 0.00f,   0.f, 0.40f,  0.0f,  0.0f, kPink,  false}},
    {"embarrassed", {0.95f, 0.75f, 0.50f, 0.10f,   0.f, 0.30f,  0.0f,  0.3f, kPink,  false}},
    {"surprised",   {0.80f, 1.30f, 0.50f, 0.00f,   0.f, 0.00f,  0.0f,  0.0f, kCyan,  false}},
    {"shocked",     {0.70f, 1.40f, 0.50f, 0.00f,   0.f, 0.00f,  0.0f,  0.0f, kCyan,  false}},
    {"thinking",    {0.90f, 0.90f, 0.35f, 0.25f,   6.f, 0.00f,  0.6f, -0.6f, kCyan,  false}},
    {"confused",    {0.95f, 1.00f, 0.40f, 0.15f,  -6.f, 0.00f, -0.6f,  0.3f, kCyan,  false}},
    {"winking",     {1.00f, 1.00f, 0.35f, 0.00f,   0.f, 0.00f,  0.0f,  0.0f, kCyan,  true }},
    {"cool",        {1.10f, 0.55f, 0.20f, 0.25f,   0.f, 0.00f,  0.0f,  0.0f, kCyan,  false}},
    {"relaxed",     {1.00f, 0.60f, 0.50f, 0.30f,   0.f, 0.20f,  0.0f,  0.0f, kCyan,  false}},
    {"delicious",   {1.00f, 0.85f, 0.50f, 0.00f,   0.f, 0.45f,  0.0f,  0.0f, kCyan,  false}},
    {"confident",   {1.00f, 0.85f, 0.30f, 0.30f,   8.f, 0.00f,  0.0f,  0.0f, kCyan,  false}},
    {"sleepy",      {1.00f, 0.50f, 0.40f, 0.50f,  -8.f, 0.00f,  0.0f,  0.2f, kCyan,  false}},
    {"dizzy",       {1.00f, 0.90f, 0.50f, 0.10f,   0.f, 0.00f,  0.0f,  0.0f, kAmber, false}},
};

// Devuelve la expresión por nombre; si no existe, "neutral".
const RobotEyes::Pose& Lookup(const char* name) {
    if (name) {
        for (const auto& n : kTable) {
            if (std::strcmp(n.name, name) == 0) {
                return n.pose;
            }
        }
    }
    return kTable[0].pose;
}

inline bool Due(uint32_t now, uint32_t t) {
    return static_cast<int32_t>(now - t) >= 0;
}

// Número aleatorio con signo en [-1, 1]
inline float RandSigned() {
    return (static_cast<int>(lv_rand(0, 200)) - 100) / 100.0f;
}

inline float Clamp1(float v) {
    return std::max(-1.0f, std::min(1.0f, v));
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

    base_ = cur_ = target_ = react_pose_ = kTable[0].pose;
    color_ = lv_color_hex(cur_.color);

    const uint32_t now = lv_tick_get();
    next_blink_ = now + lv_rand(1500, 3500);
    next_gaze_ = now + lv_rand(1000, 2500);

    Apply();
    timer_ = lv_timer_create(TimerCb, kTickMs, this);

    // Tarea que lee el sensor de movimiento
    imu_running_ = true;
    if (xTaskCreate(ImuTask, "eyes_imu", 4096, this, 2, nullptr) != pdPASS) {
        imu_running_ = false;
        ESP_LOGW(TAG, "No se pudo crear la tarea del sensor de movimiento");
    }
}

RobotEyes::~RobotEyes() {
    imu_stop_ = true;
    for (int i = 0; i < 25 && imu_running_; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
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
    base_ = Lookup(emotion);

    // Un parpadeo rápido al cambiar de expresión queda más natural.
    blinking_ = true;
    blink_end_ = lv_tick_get() + 110;
}

void RobotEyes::StartReaction(const char* name, uint32_t ms) {
    react_pose_ = Lookup(name);
    react_active_ = true;
    react_hold_ = (ms == 0);
    react_until_ = lv_tick_get() + ms;
}

void RobotEyes::TimerCb(lv_timer_t* t) {
    static_cast<RobotEyes*>(lv_timer_get_user_data(t))->Tick();
}

void RobotEyes::ImuTask(void* arg) {
    RobotEyes* self = static_cast<RobotEyes*>(arg);
    self->ImuLoop();
    self->imu_running_ = false;
    vTaskDelete(nullptr);
}

// Lee el acelerómetro QMI8658 (mismo bus I2C que usa el resto de la placa).
void RobotEyes::ImuLoop() {
    // Esperar a que arranque el resto de la placa
    vTaskDelay(pdMS_TO_TICKS(1500));

    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_master_get_bus_handle(I2C_NUM_0, &bus) != ESP_OK || bus == nullptr) {
        ESP_LOGW(TAG, "No se encontró el bus I2C del sensor");
        imu_status_ = 2;
        return;
    }

    // El QMI8658 puede estar en 0x6B o 0x6A
    uint8_t addr = 0;
    const uint8_t candidates[] = {0x6B, 0x6A};
    for (uint8_t a : candidates) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            addr = a;
            break;
        }
    }
    if (addr == 0) {
        ESP_LOGW(TAG, "QMI8658 no responde en 0x6B ni 0x6A");
        imu_status_ = 2;
        return;
    }

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr;
    cfg.scl_speed_hz = 400000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK || dev == nullptr) {
        ESP_LOGW(TAG, "No se pudo añadir el QMI8658 al bus");
        imu_status_ = 2;
        return;
    }

    // Comprobar identidad (WHO_AM_I = 0x05)
    uint8_t reg = 0x00;
    uint8_t id = 0;
    if (i2c_master_transmit_receive(dev, &reg, 1, &id, 1, 100) != ESP_OK || id != 0x05) {
        ESP_LOGW(TAG, "El chip en 0x%02X no es un QMI8658 (id=0x%02X)", addr, id);
        i2c_master_bus_rm_device(dev);
        imu_status_ = 2;
        return;
    }

    auto write_reg = [&](uint8_t r, uint8_t v) {
        uint8_t b[2] = {r, v};
        return i2c_master_transmit(dev, b, 2, 100);
    };
    write_reg(0x02, 0x40);  // CTRL1: autoincremento de dirección
    write_reg(0x03, 0x16);  // CTRL2: acelerómetro +-4 g, 125 Hz
    write_reg(0x08, 0x01);  // CTRL7: acelerómetro activado
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "QMI8658 listo en 0x%02X", addr);
    imu_status_ = 1;

    constexpr float kLsbPerG = 8192.0f;  // +-4 g
    float tilt_f = 0.0f;
    int spikes = 0;
    int64_t first_spike = 0;
    int64_t last_spike = 0;
    int64_t last_shake = -100000;

    while (!imu_stop_) {
        uint8_t r = 0x35;  // AX_L ... AZ_H
        uint8_t d[6] = {};
        if (i2c_master_transmit_receive(dev, &r, 1, d, 6, 50) == ESP_OK) {
            const int16_t rx = static_cast<int16_t>(d[0] | (d[1] << 8));
            const int16_t ry = static_cast<int16_t>(d[2] | (d[3] << 8));
            const int16_t rz = static_cast<int16_t>(d[4] | (d[5] << 8));
            const float ax = rx / kLsbPerG;
            const float ay = ry / kLsbPerG;
            const float az = rz / kLsbPerG;

            // --- Inclinación lateral ---
            float t = (kTiltUseY ? ay : ax) * kTiltSign;
            if (std::fabs(t) < kTiltDeadG) {
                t = 0.0f;
            }
            tilt_f = tilt_f * 0.8f + t * 0.2f;
            tilt_in_ = Clamp1(tilt_f / kTiltFullG);

            // --- Sacudida: varios golpes fuertes seguidos ---
            const int64_t now_ms = esp_timer_get_time() / 1000;
            const float m = std::sqrt(ax * ax + ay * ay + az * az);
            if (spikes > 0 && now_ms - first_spike > static_cast<int64_t>(kShakeWindowMs)) {
                spikes = 0;
            }
            if (std::fabs(m - 1.0f) > kShakeThresholdG && now_ms - last_spike >= 60) {
                if (spikes == 0) {
                    first_spike = now_ms;
                }
                spikes++;
                last_spike = now_ms;
                if (spikes >= kShakeSpikes && now_ms - last_shake >= static_cast<int64_t>(kShakeCooldownMs)) {
                    shake_req_ = true;
                    last_shake = now_ms;
                    spikes = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    i2c_master_bus_rm_device(dev);
}

void RobotEyes::PollTouch(uint32_t now) {
    // Buscar el dispositivo táctil (puntero) de LVGL
    if (indev_ == nullptr) {
        for (lv_indev_t* i = lv_indev_get_next(nullptr); i != nullptr; i = lv_indev_get_next(i)) {
            if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {
                indev_ = i;
                break;
            }
        }
    }
    if (indev_ == nullptr) {
        return;
    }

    const bool pressed = (lv_indev_get_state(indev_) == LV_INDEV_STATE_PRESSED);
    lv_point_t p = {0, 0};
    if (pressed) {
        lv_indev_get_point(indev_, &p);
    }

    lv_display_t* disp = lv_display_get_default();
    const float hres = disp ? static_cast<float>(lv_display_get_horizontal_resolution(disp)) : static_cast<float>(width_);
    const float vres = disp ? static_cast<float>(lv_display_get_vertical_resolution(disp)) : static_cast<float>(height_);

    // Dedo recién puesto
    if (pressed && !was_pressed_) {
        press_start_ = now;
        press_x_ = p.x;
        press_y_ = p.y;
        moved_ = false;
        hold_stage_ = 0;
    }

    if (pressed) {
        touching_ = true;

        // Los ojos miran hacia donde está el dedo
        touch_tx_ = Clamp1((p.x - hres * 0.5f) / (hres * 0.5f));
        touch_ty_ = Clamp1((p.y - vres * 0.5f) / (vres * 0.5f));

        // ¿Se está arrastrando?
        const int dx = p.x - press_x_;
        const int dy = p.y - press_y_;
        if (dx * dx + dy * dy > kMovePx * kMovePx) {
            moved_ = true;
        }

        // Pulsaciones largas (solo si el dedo está quieto)
        if (!moved_) {
            const uint32_t held = now - press_start_;
            if (held >= kSleepPressMs && hold_stage_ < 2) {
                StartReaction("sleepy", 0);
                hold_stage_ = 2;
            } else if (held >= kLongPressMs && hold_stage_ < 1) {
                StartReaction("loving", 0);
                hold_stage_ = 1;
            }
        }
    } else {
        touching_ = false;
        touch_tx_ = 0.0f;
        touch_ty_ = 0.0f;
    }

    // Dedo recién levantado
    if (!pressed && was_pressed_) {
        const uint32_t held = now - press_start_;
        if (hold_stage_ > 0) {
            // Caricia o sueño: se mantiene un momento y vuelve
            react_hold_ = false;
            react_until_ = now + kReleaseHoldMs;
        } else if (!moved_ && held < kTapMaxMs) {
            const bool dbl = tapped_before_ && (now - last_tap_) < kDoubleTapMs;
            last_tap_ = now;
            tapped_before_ = true;
            if (dbl) {
                StartReaction("laughing", kDoubleReactMs);
            } else {
                StartReaction("surprised", kTapReactMs);
            }
        }
    }

    was_pressed_ = pressed;
}

void RobotEyes::Tick() {
    const uint32_t now = lv_tick_get();

    // --- Tacto ---
    PollTouch(now);

    // --- Sensor de movimiento ---
    const int st = imu_status_.exchange(0);
    if (st == 1) {
        StartReaction("winking", 1200);  // guiño: el sensor funciona
    } else if (st == 2) {
        StartReaction("sad", 1500);      // cara triste: no se encontró el sensor
    }
    if (shake_req_.exchange(false)) {
        shaking_ = true;
        shake_start_ = now;
        shake_until_ = now + kShakeDurationMs;
        StartReaction("dizzy", kShakeDurationMs);
    }
    if (shaking_) {
        const float t = (now - shake_start_) / 1000.0f;
        const float remain = static_cast<int32_t>(shake_until_ - now) / 1000.0f;
        const float amp = remain > 0.7f ? 1.0f : std::max(0.0f, remain / 0.7f);
        shake_tx_ = std::cos(t * 9.0f) * amp;
        shake_ty_ = std::sin(t * 9.0f) * amp * 0.7f;
        if (Due(now, shake_until_)) {
            shaking_ = false;
            shake_tx_ = 0.0f;
            shake_ty_ = 0.0f;
        }
    }

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

    // --- ¿Reacción activa? ---
    if (react_active_ && !react_hold_ && Due(now, react_until_)) {
        react_active_ = false;
    }
    target_ = react_active_ ? react_pose_ : base_;

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
    ease(touch_gx_, touch_tx_, 0.35f);
    ease(touch_gy_, touch_ty_, 0.35f);
    ease(touch_w_, touching_ ? 1.0f : 0.0f, 0.25f);
    ease(tilt_gx_, tilt_in_.load(), 0.2f);
    ease(shake_gx_, shake_tx_, 0.5f);
    ease(shake_gy_, shake_ty_, 0.5f);
    color_ = lv_color_mix(lv_color_hex(target_.color), color_, 70);

    Apply();
}

void RobotEyes::Apply() {
    const float base_w = width_ * 0.26f;
    const float base_h = height_ * 0.62f;
    const float gap = width_ * 0.10f;

    const float w = std::max(base_w * cur_.w, 4.0f);
    const float h_open = base_h * cur_.h;

    // La mirada errante se apaga cuando hay dedo, inclinación o mareo
    float idle_k = (1.0f - touch_w_) * (1.0f - std::min(1.0f, std::fabs(tilt_gx_)));
    if (shaking_) {
        idle_k = 0.0f;
    }
    const float tilt_w = 1.0f - touch_w_;  // el dedo manda sobre la inclinación

    const float gx = (cur_.gaze_x * 0.12f + idle_x_ * idle_k) * base_w
                     + touch_gx_ * kTouchGazeX * base_w
                     + tilt_gx_ * kTiltGazeX * base_w * tilt_w
                     + shake_gx_ * 0.35f * base_w;
    const float gy = (cur_.gaze_y * 0.12f + idle_y_ * idle_k) * base_h
                     + touch_gy_ * kTouchGazeY * base_h
                     + shake_gy_ * 0.25f * base_h;

    for (int i = 0; i < 2; ++i) {
        const bool right = (i == 1);
        Eye& e = eyes_[i];

        float k = blink_k_;
        if (right) k = std::min(k, wink_k_);
        const float hh = std::max(h_open * k, 3.0f);

        // Posición: centro del área +/- mitad del ojo y del hueco, más la mirada.
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
