#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wformat-overflow"
#include "clock_app.h"
#include "application.h"
#include "display/display.h"
#include "radio.h"

#if __has_include("assets/lang_config.h")
#include "assets/lang_config.h"
#define CLOCK_HAVE_SOUND 1
#endif

#include <dirent.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define TAG "ClockApp"

namespace {

constexpr int kW = 480;
constexpr int kH = 480;

constexpr uint32_t kPanel = 0x1A2238;
constexpr uint32_t kDim = 0x14182A;
constexpr uint32_t kAccent = 0x3A8DDE;

constexpr int kSnoozeMin = 5;   // minutos de "posponer"
constexpr int kStartVol = 10;   // el volumen de la alarma sube poco a poco: de 10 %...
constexpr int kMaxVol = 80;     // ...a 80 %, 2 puntos por segundo
constexpr int kRingMaxSecs = 600;

// Cambia esta fuente si el texto se ve pequeno
const lv_font_t* kFont = LV_FONT_DEFAULT;

const char* kMonths[12] = {"Enero", "Febrero", "Marzo", "Abril", "Mayo", "Junio",
                           "Julio", "Agosto", "Septiembre", "Octubre", "Noviembre", "Diciembre"};
const char* kWeekdays[7] = {"Domingo", "Lunes", "Martes", "Miercoles", "Jueves", "Viernes", "Sabado"};
const char* kDayLetters[7] = {"L", "M", "X", "J", "V", "S", "D"};

const uint8_t kSegMask[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

void Show(lv_obj_t* o, bool on) {
    if (!o) return;
    const bool hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (on && hidden) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else if (!on && !hidden) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

void SetLabel(lv_obj_t* l, const char* text) {
    if (strcmp(lv_label_get_text(l), text) != 0) lv_label_set_text(l, text);
}

lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t color, int radius) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* Circle(lv_obj_t* parent, int x, int y, int d, lv_color_t color) {
    return Box(parent, x, y, d, d, color, LV_RADIUS_CIRCLE);
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, uint32_t color, lv_align_t align, int x,
                    int y, int width = 0, bool left = false) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, C(color), 0);
    lv_obj_set_style_text_font(l, kFont, 0);
    lv_obj_set_style_text_align(l, left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    if (width > 0) lv_obj_set_width(l, width);
    lv_obj_align(l, align, x, y);
    return l;
}

void OnEvent(lv_event_t* e) {
    ClockApp::GetInstance().HandleButton((int)(intptr_t)lv_event_get_user_data(e));
}

void OnTimer(lv_timer_t*) { ClockApp::GetInstance().Tick(); }

// Toques sobre la pantalla del reloj: sirven para detectar el deslizamiento lateral
void OnTouchEvent(lv_event_t* e) {
    lv_indev_t* indev = lv_event_get_indev(e);
    lv_point_t p = {0, 0};
    if (indev) lv_indev_get_point(indev, &p);
    ClockApp::GetInstance().OnTouch(lv_event_get_code(e) == LV_EVENT_PRESSED, (int)p.x, (int)p.y);
}

lv_obj_t* MakeButton(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius,
                     const char* text, int id, uint32_t text_color = 0x101018) {
    lv_obj_t* b = Box(parent, x, y, w, h, C(color), radius);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, OnEvent, LV_EVENT_CLICKED, (void*)(intptr_t)id);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, C(text_color), 0);
    lv_obj_set_style_text_font(l, kFont, 0);
    lv_obj_center(l);
    return b;
}

void SetButtonText(lv_obj_t* b, const char* text) {
    lv_obj_t* l = lv_obj_get_child(b, 0);
    if (l) SetLabel(l, text);
}

lv_obj_t* MakePanel(lv_obj_t* parent) {
    lv_obj_t* p = Box(parent, 0, 0, kW, kH, C(0x000000), 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

// Cifra de 7 segmentos
void MakeSegs(lv_obj_t* parent, lv_obj_t** seg, int x, int y, int w, int h, int t, uint32_t off) {
    const int uh = (h - 3 * t) / 2;
    const int hw = w - 2 * t;
    const lv_color_t c = C(off);
    seg[0] = Box(parent, x + t, y, hw, t, c, t / 3 + 1);
    seg[1] = Box(parent, x + w - t, y + t, t, uh, c, t / 3 + 1);
    seg[2] = Box(parent, x + w - t, y + 2 * t + uh, t, uh, c, t / 3 + 1);
    seg[3] = Box(parent, x + t, y + h - t, hw, t, c, t / 3 + 1);
    seg[4] = Box(parent, x, y + 2 * t + uh, t, uh, c, t / 3 + 1);
    seg[5] = Box(parent, x, y + t, t, uh, c, t / 3 + 1);
    seg[6] = Box(parent, x + t, y + t + uh, hw, t, c, t / 3 + 1);
}

// v: 0-9 cifra, -1 guion, otro = apagado
void SetSegs(lv_obj_t** seg, uint32_t on, uint32_t off, int v) {
    uint8_t mask = 0;
    if (v >= 0 && v <= 9) {
        mask = kSegMask[v];
    } else if (v == -1) {
        mask = 0x40;
    }
    for (int i = 0; i < 7; i++) {
        lv_obj_set_style_bg_color(seg[i], C(((mask >> i) & 1) ? on : off), 0);
    }
}

// Pone una mano del reloj analogico (hilera de puntos) apuntando a un angulo (0 = las 12, en radianes)
void PlaceHand(lv_obj_t** dots, int n, int d, float angle, int r0, int r1) {
    for (int k = 0; k < n; k++) {
        const float r = (float)r0 + (float)(r1 - r0) * (float)k / (float)(n > 1 ? n - 1 : 1);
        const int x = 240 + (int)(r * sinf(angle)) - d / 2;
        const int y = 240 - (int)(r * cosf(angle)) - d / 2;
        lv_obj_set_pos(dots[k], x, y);
    }
}

// Ruta real de un MP3 de alarma (carpeta /sdcard/alarmas o raiz de la SD); vacia si no existe
std::string ResolveMp3(const char* file) {
    if (!file || !file[0]) return "";
    if (!Radio::GetInstance().SdReady()) return "";
    const std::string a = std::string("/sdcard/alarmas/") + file;
    FILE* f = fopen(a.c_str(), "rb");
    if (f) {
        fclose(f);
        return a;
    }
    const std::string b = std::string("/sdcard/") + file;
    f = fopen(b.c_str(), "rb");
    if (f) {
        fclose(f);
        return b;
    }
    return "";
}

}  // namespace

ClockApp& ClockApp::GetInstance() {
    static ClockApp instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Inicio y ajustes
// ---------------------------------------------------------------------------

void ClockApp::Init(Display* display) {
    if (root_ || !display) return;
    display_ = display;
    DisplayLockGuard lock(display_);
    LoadSettings();
    ReadTime();
    BuildUI();
    ShowStyle();
    RefreshAlarmList();
    UpdateAlarmLabel();
    lv_timer_create(OnTimer, 1000, nullptr);
    ESP_LOGI(TAG, "Reloj listo");
}

void ClockApp::Activity() {
    if (on_activity_) on_activity_();
}

void ClockApp::LoadSettings() {
    for (int i = 0; i < kMaxAlarms; i++) {  // valores por defecto
        memset(&alarms_[i], 0, sizeof(Alarm));
        alarms_[i].hour = 7;
        alarms_[i].minute = 0;
        alarms_[i].days = 0x1F;  // lunes a viernes
    }
    nvs_handle_t h;
    if (nvs_open("clockapp", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    if (nvs_get_i32(h, "style", &v) == ESP_OK && v >= 0 && v < kStyles) style_ = (int)v;
    Alarm tmp[kMaxAlarms];
    size_t size = sizeof(tmp);
    if (nvs_get_blob(h, "alarms", tmp, &size) == ESP_OK && size == sizeof(tmp)) {
        for (int i = 0; i < kMaxAlarms; i++) {
            if (tmp[i].hour < 24 && tmp[i].minute < 60 && tmp[i].src < 3) {
                alarms_[i] = tmp[i];
                alarms_[i].file[sizeof(alarms_[i].file) - 1] = 0;
            }
        }
    }
    nvs_close(h);
}

void ClockApp::SaveSettings() {
    nvs_handle_t h;
    if (nvs_open("clockapp", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "style", style_);
    nvs_set_blob(h, "alarms", alarms_, sizeof(alarms_));
    nvs_commit(h);
    nvs_close(h);
}

void ClockApp::ReadTime() {
    const time_t now = time(nullptr);
    if (now >= 1700000000) {
        struct tm t;
        localtime_r(&now, &t);
        time_ok_ = true;
        h_ = t.tm_hour;
        m_ = t.tm_min;
        s_ = t.tm_sec;
        wday_ = t.tm_wday;
        mday_ = t.tm_mday;
        mon_ = t.tm_mon + 1;
        year_ = t.tm_year + 1900;
    } else {
        time_ok_ = false;
    }
}

// Pasados 2 minutos desde el arranque la hora ya esta sincronizada por internet
bool ClockApp::TimeTrusted() const { return esp_timer_get_time() > 120LL * 1000000LL; }

void ClockApp::SetVisible(bool visible, bool auto_standby) {
    if (!display_ || !root_) return;
    DisplayLockGuard lock(display_);
    visible_ = visible;
    auto_standby_ = visible && auto_standby;
    Show(root_, visible);
    if (visible) {
        Show(alarm_panel_, false);
        Show(edit_panel_, false);
        ShowStyle();
        UpdateFace(true);
        UpdateAlarmLabel();
    } else if (test_playing_) {
        StopSound();
        test_playing_ = false;
    }
    if (!auto_standby) Activity();  // el reposo automatico no debe despertar la pantalla
}

// ---------------------------------------------------------------------------
// Bucle y toques
// ---------------------------------------------------------------------------

void ClockApp::Tick() {
    if (!root_) return;
    ReadTime();
    CheckAlarms();
    if (ringing_) RingTick();

    // Si el asistente empieza a hablar o escuchar, se sale del reposo automatico
    if (visible_ && auto_standby_ &&
        Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        if (on_exit_) on_exit_();
        return;
    }
    if (!visible_) return;
    UpdateFace(false);
}

void ClockApp::OnTouch(bool pressed, int x, int y) {
    if (pressed) {
        touch_x0_ = x;
        touch_y0_ = y;
        return;
    }
    const int dx = x - touch_x0_;
    const int dy = y - touch_y0_;
    if (abs(dx) > 70 && abs(dx) > 2 * abs(dy)) {  // deslizar: cambia de estilo
        style_ = (style_ + (dx < 0 ? 1 : kStyles - 1)) % kStyles;
        ShowStyle();
        UpdateFace(true);
        SaveSettings();
        Activity();
        return;
    }
    if (abs(dx) < 25 && abs(dy) < 25) {  // toque
        Activity();
        if (auto_standby_ && on_exit_) on_exit_();
    }
}

// ---------------------------------------------------------------------------
// Interfaz
// ---------------------------------------------------------------------------

void ClockApp::MakeDigit(Digit& d, lv_obj_t* parent, int x, int y, int w, int h, int t,
                         uint32_t on, uint32_t off) {
    d.on = on;
    d.off = off;
    MakeSegs(parent, d.seg, x, y, w, h, t, off);
}

void ClockApp::SetDigit(Digit& d, int v) { SetSegs(d.seg, d.on, d.off, v); }

void ClockApp::BuildUI() {
    root_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, kW, kH);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, C(0x000000), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(root_, OnTouchEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(root_, OnTouchEvent, LV_EVENT_RELEASED, nullptr);

    for (int i = 0; i < kStyles; i++) {
        face_[i] = Box(root_, 0, 0, kW, kH, C(0x000000), 0);
        lv_obj_set_style_bg_opa(face_[i], LV_OPA_TRANSP, 0);
    }

    const lv_color_t white = C(0xFFFFFF);

    // --- Estilo 0: digital grande con barra de segundos ---
    {
        lv_obj_t* f = face_[0];
        const int x0 = 66, y0 = 120;
        MakeDigit(d0_[0], f, x0, y0, 70, 130, 14, 0xFFFFFF, kDim);
        MakeDigit(d0_[1], f, x0 + 82, y0, 70, 130, 14, 0xFFFFFF, kDim);
        colon0_[0] = Circle(f, x0 + 167, y0 + 36, 14, white);
        colon0_[1] = Circle(f, x0 + 167, y0 + 80, 14, white);
        MakeDigit(d0_[2], f, x0 + 196, y0, 70, 130, 14, 0xFFFFFF, kDim);
        MakeDigit(d0_[3], f, x0 + 278, y0, 70, 130, 14, 0xFFFFFF, kDim);
        Box(f, x0, y0 + 150, 348, 6, C(kPanel), 3);
        sec_fill_ = Box(f, x0, y0 + 150, 2, 6, C(kAccent), 3);
        date0_ = MakeLabel(f, "", 0xB8C4E0, LV_ALIGN_TOP_MID, 0, y0 + 180, 420);
    }

    // --- Estilo 1: analogico ---
    {
        lv_obj_t* f = face_[1];
        for (int i = 0; i < 12; i++) {
            const float a = (float)i * 0.523599f;
            const int d = (i % 3 == 0) ? 16 : 9;
            Circle(f, 240 + (int)(178.0f * sinf(a)) - d / 2, 240 - (int)(178.0f * cosf(a)) - d / 2, d,
                   C((i % 3 == 0) ? 0xFFFFFF : 0x8FA3C8));
        }
        date1_ = MakeLabel(f, "", 0x8FA3C8, LV_ALIGN_TOP_MID, 0, 316, 300);
        for (int k = 0; k < 9; k++) hand_h_[k] = Circle(f, 0, 0, 12, white);
        for (int k = 0; k < 12; k++) hand_m_[k] = Circle(f, 0, 0, 9, C(0xDDE6FF));
        for (int k = 0; k < 20; k++) hand_s_[k] = Circle(f, 0, 0, 5, C(0xFF4D4D));
        Circle(f, 240 - 9, 240 - 9, 18, C(0xFF4D4D));
    }

    // --- Estilo 2: anillo de segundos con la hora en el centro ---
    {
        lv_obj_t* f = face_[2];
        for (int i = 0; i < 60; i++) {
            const float a = (float)i * 0.104720f;
            ring_dot_[i] = Circle(f, 240 + (int)(206.0f * sinf(a)) - 4, 240 - (int)(206.0f * cosf(a)) - 4,
                                  8, C(kDim));
        }
        const int x0 = 130, y0 = 188;
        MakeDigit(d2_[0], f, x0, y0, 44, 84, 9, 0xFFFFFF, kDim);
        MakeDigit(d2_[1], f, x0 + 52, y0, 44, 84, 9, 0xFFFFFF, kDim);
        colon2_[0] = Circle(f, 235, y0 + 22, 10, white);
        colon2_[1] = Circle(f, 235, y0 + 52, 10, white);
        MakeDigit(d2_[2], f, x0 + 124, y0, 44, 84, 9, 0xFFFFFF, kDim);
        MakeDigit(d2_[3], f, x0 + 176, y0, 44, 84, 9, 0xFFFFFF, kDim);
        date2_ = MakeLabel(f, "", 0x8FA3C8, LV_ALIGN_TOP_MID, 0, 292, 300);
    }

    // --- Estilo 3: tarjetas ---
    {
        lv_obj_t* f = face_[3];
        Box(f, 62, 118, 160, 220, C(kPanel), 24);
        Box(f, 258, 118, 160, 220, C(kPanel), 24);
        MakeDigit(d3_[0], f, 62 + 20, 173, 52, 110, 12, 0xFFFFFF, 0x262E48);
        MakeDigit(d3_[1], f, 62 + 84, 173, 52, 110, 12, 0xFFFFFF, 0x262E48);
        MakeDigit(d3_[2], f, 258 + 20, 173, 52, 110, 12, 0xFFFFFF, 0x262E48);
        MakeDigit(d3_[3], f, 258 + 84, 173, 52, 110, 12, 0xFFFFFF, 0x262E48);
        Box(f, 62, 226, 160, 4, C(0x000000), 0);
        Box(f, 258, 226, 160, 4, C(0x000000), 0);
        date3_ = MakeLabel(f, "", 0xB8C4E0, LV_ALIGN_TOP_MID, 0, 362, 420);
    }

    // --- Estilo 4: noche (rojo tenue, para la mesita) ---
    {
        lv_obj_t* f = face_[4];
        const int x0 = 79, y0 = 165;
        MakeDigit(d4_[0], f, x0, y0, 60, 110, 12, 0x9A1818, 0x1A0707);
        MakeDigit(d4_[1], f, x0 + 74, y0, 60, 110, 12, 0x9A1818, 0x1A0707);
        colon4_[0] = Circle(f, x0 + 160, y0 + 30, 12, C(0x9A1818));
        colon4_[1] = Circle(f, x0 + 160, y0 + 68, 12, C(0x9A1818));
        MakeDigit(d4_[2], f, x0 + 188, y0, 60, 110, 12, 0x9A1818, 0x1A0707);
        MakeDigit(d4_[3], f, x0 + 262, y0, 60, 110, 12, 0x9A1818, 0x1A0707);
        date4_ = MakeLabel(f, "", 0x5A1212, LV_ALIGN_TOP_MID, 0, 300, 420);
    }

    // --- Elementos comunes ---
    for (int i = 0; i < kStyles; i++) {
        style_dot_[i] = Circle(root_, 240 + (i - 2) * 18 - 4, 454, 8, C(0x3A4560));
    }
    alarm_lbl_ = MakeLabel(root_, "", 0x6F7FA5, LV_ALIGN_TOP_MID, 0, 26, 300);
    bell_btn_ = MakeButton(root_, 345, 400, 90, 34, kPanel, 12, "ALARMA", 1, 0xFFFFFF);
    lv_obj_set_style_bg_opa(bell_btn_, LV_OPA_70, 0);

    // --- Panel de alarmas ---
    alarm_panel_ = MakePanel(root_);
    MakeLabel(alarm_panel_, "Despertador", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 26, 380);
    for (int i = 0; i < kMaxAlarms; i++) {
        lv_obj_t* row = Box(alarm_panel_, 40, 62 + i * 76, 400, 68, C(kPanel), 12);
        row_lbl_[i] = MakeLabel(row, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 12, 10, 250, true);
        row_tog_[i] = MakeButton(row, 270, 17, 56, 34, 0x59627A, 10, "OFF", 60 + i, 0xFFFFFF);
        MakeButton(row, 334, 17, 56, 34, 0x2C3552, 10, "EDIT", 70 + i, 0xFFFFFF);
    }
    MakeLabel(alarm_panel_, "MP3 de la alarma: carpeta /sdcard/alarmas de la tarjeta SD", 0x6F7FA5,
              LV_ALIGN_TOP_MID, 0, 372, 400);
    MakeButton(alarm_panel_, 140, 410, 200, 48, kPanel, 14, "VOLVER", 80, 0xFFFFFF);

    // --- Editor de una alarma ---
    edit_panel_ = MakePanel(root_);
    ed_title_ = MakeLabel(edit_panel_, "", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 22, 380);
    MakeLabel(edit_panel_, "Hora", 0x8FA3C8, LV_ALIGN_TOP_LEFT, 30, 76, 70, true);
    MakeButton(edit_panel_, 110, 64, 56, 40, kPanel, 10, "-", 90, 0xFFFFFF);
    ed_hour_ = MakeLabel(edit_panel_, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 170, 76, 60);
    MakeButton(edit_panel_, 234, 64, 56, 40, kPanel, 10, "+", 91, 0xFFFFFF);
    MakeLabel(edit_panel_, "Minutos", 0x8FA3C8, LV_ALIGN_TOP_LEFT, 30, 124, 70, true);
    MakeButton(edit_panel_, 110, 112, 48, 40, kPanel, 10, "-5", 92, 0xFFFFFF);
    MakeButton(edit_panel_, 162, 112, 48, 40, kPanel, 10, "-1", 93, 0xFFFFFF);
    ed_min_ = MakeLabel(edit_panel_, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 214, 124, 50);
    MakeButton(edit_panel_, 268, 112, 48, 40, kPanel, 10, "+1", 94, 0xFFFFFF);
    MakeButton(edit_panel_, 320, 112, 48, 40, kPanel, 10, "+5", 95, 0xFFFFFF);
    MakeLabel(edit_panel_, "Dias", 0x8FA3C8, LV_ALIGN_TOP_LEFT, 30, 176, 70, true);
    for (int i = 0; i < 7; i++) {
        ed_day_[i] = MakeButton(edit_panel_, 90 + i * 47, 164, 42, 36, kPanel, 8, kDayLetters[i], 100 + i,
                                0xFFFFFF);
    }
    ed_src_ = MakeButton(edit_panel_, 50, 214, 380, 40, kPanel, 10, "", 110, 0xFFFFFF);
    MakeButton(edit_panel_, 50, 262, 56, 40, kPanel, 10, "<", 111, 0xFFFFFF);
    ed_sel_ = MakeLabel(edit_panel_, "", 0xFFE066, LV_ALIGN_TOP_LEFT, 112, 273, 256);
    MakeButton(edit_panel_, 374, 262, 56, 40, kPanel, 10, ">", 112, 0xFFFFFF);
    ed_test_ = MakeButton(edit_panel_, 50, 330, 112, 48, 0x8B5CF6, 12, "PROBAR", 113, 0xFFFFFF);
    MakeButton(edit_panel_, 184, 330, 112, 48, 0x1DD1A1, 12, "GUARDAR", 114);
    MakeButton(edit_panel_, 318, 330, 112, 48, kPanel, 12, "CANCELAR", 115, 0xFFFFFF);
    MakeLabel(edit_panel_, "Dias sin marcar = suena una sola vez", 0x6F7FA5, LV_ALIGN_TOP_MID, 0, 392, 400);

    // --- Aviso a pantalla completa (encima de todo) ---
    ring_panel_ = Box(lv_layer_top(), 0, 0, kW, kH, C(0x7A1020), 0);
    lv_obj_add_flag(ring_panel_, LV_OBJ_FLAG_CLICKABLE);
    Show(ring_panel_, false);
    MakeLabel(ring_panel_, "DESPERTADOR", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 70, 380);
    const int ax = 128;
    MakeDigit(ring_d_[0], ring_panel_, ax, 110, 44, 80, 9, 0xFFFFFF, 0x4A0A16);
    MakeDigit(ring_d_[1], ring_panel_, ax + 52, 110, 44, 80, 9, 0xFFFFFF, 0x4A0A16);
    Circle(ring_panel_, ax + 106, 134, 12, white);
    Circle(ring_panel_, ax + 106, 160, 12, white);
    MakeDigit(ring_d_[2], ring_panel_, ax + 128, 110, 44, 80, 9, 0xFFFFFF, 0x4A0A16);
    MakeDigit(ring_d_[3], ring_panel_, ax + 180, 110, 44, 80, 9, 0xFFFFFF, 0x4A0A16);
    ring_txt_ = MakeLabel(ring_panel_, "", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 226, 380);
    MakeButton(ring_panel_, 50, 320, 180, 64, 0xFFFFFF, 20, "POSPONER 5 MIN", 120, 0x7A1020);
    MakeButton(ring_panel_, 250, 320, 180, 64, 0xFFFFFF, 20, "PARAR", 121, 0x7A1020);
}

void ClockApp::ShowStyle() {
    for (int i = 0; i < kStyles; i++) {
        Show(face_[i], i == style_);
        lv_obj_set_style_bg_color(style_dot_[i], C(i == style_ ? 0xFFFFFF : 0x3A4560), 0);
    }
    const bool night = (style_ == 4);
    lv_obj_set_style_text_color(alarm_lbl_, C(night ? 0x5A1212 : 0x6F7FA5), 0);
}

void ClockApp::UpdateFace(bool force) {
    ReadTime();
    const bool minute_changed = force || m_ != last_min_;
    last_min_ = m_;

    char date[80];
    if (time_ok_) {
        snprintf(date, sizeof(date), "%s %d de %s de %d", kWeekdays[wday_], mday_, kMonths[mon_ - 1], year_);
    } else {
        snprintf(date, sizeof(date), "Sin hora (conecta al wifi)");
    }

    auto set4 = [&](Digit* d) {
        if (time_ok_) {
            SetDigit(d[0], h_ / 10);
            SetDigit(d[1], h_ % 10);
            SetDigit(d[2], m_ / 10);
            SetDigit(d[3], m_ % 10);
        } else {
            for (int i = 0; i < 4; i++) SetDigit(d[i], -1);
        }
    };
    const bool blink = (!time_ok_) || (s_ % 2 == 0);

    switch (style_) {
        case 0:
            if (minute_changed) {
                set4(d0_);
                SetLabel(date0_, date);
            }
            Show(colon0_[0], blink);
            Show(colon0_[1], blink);
            lv_obj_set_width(sec_fill_, std::max(2, (348 * (time_ok_ ? s_ : 0)) / 60));
            break;
        case 1: {
            const float sa = (float)(time_ok_ ? s_ : 0) * 0.104720f;
            const float ma = ((float)(time_ok_ ? m_ : 0) + (float)(time_ok_ ? s_ : 0) / 60.0f) * 0.104720f;
            const float ha = ((float)(time_ok_ ? h_ % 12 : 0) + (float)(time_ok_ ? m_ : 0) / 60.0f) * 0.523599f;
            PlaceHand(hand_h_, 9, 12, ha, 20, 100);
            PlaceHand(hand_m_, 12, 9, ma, 20, 152);
            PlaceHand(hand_s_, 20, 5, sa, 10, 172);
            if (minute_changed) SetLabel(date1_, date);
            break;
        }
        case 2:
            for (int i = 0; i < 60; i++) {
                lv_obj_set_style_bg_color(ring_dot_[i],
                                          C((time_ok_ && i <= s_) ? kAccent : kDim), 0);
            }
            if (minute_changed) {
                set4(d2_);
                SetLabel(date2_, date);
            }
            Show(colon2_[0], blink);
            Show(colon2_[1], blink);
            break;
        case 3:
            if (minute_changed) {
                set4(d3_);
                SetLabel(date3_, date);
            }
            break;
        default:
            if (minute_changed) {
                set4(d4_);
                SetLabel(date4_, date);
            }
            Show(colon4_[0], blink);
            Show(colon4_[1], blink);
            break;
    }
    if (minute_changed) UpdateAlarmLabel();
}

// Texto "Alarma 07:30" con la proxima alarma activa
void ClockApp::UpdateAlarmLabel() {
    int best = 1 << 30, bi = -1;
    bool any = false;
    for (int i = 0; i < kMaxAlarms; i++) {
        const Alarm& a = alarms_[i];
        if (!a.enabled) continue;
        any = true;
        if (!time_ok_) continue;
        const int now_min = h_ * 60 + m_;
        for (int d = 0; d <= 7; d++) {
            const int wd = ((wday_ + 6) % 7 + d) % 7;  // 0 = lunes
            if (a.days == 0 || ((a.days >> wd) & 1)) {
                const int minutes = d * 1440 + a.hour * 60 + a.minute - now_min;
                if (minutes > 0) {
                    if (minutes < best) {
                        best = minutes;
                        bi = i;
                    }
                    break;
                }
            }
        }
    }
    any_enabled_ = any;
    char buf[40];
    if (bi >= 0) {
        snprintf(buf, sizeof(buf), "Alarma %02d:%02d", alarms_[bi].hour, alarms_[bi].minute);
        SetLabel(alarm_lbl_, buf);
        Show(alarm_lbl_, true);
    } else {
        Show(alarm_lbl_, false);
    }
}

// ---------------------------------------------------------------------------
// Lista de alarmas y editor
// ---------------------------------------------------------------------------

std::string ClockApp::DaysText(const Alarm& a) const {
    if (a.days == 0) return "Una vez";
    if (a.days == 0x7F) return "Todos los dias";
    std::string s;
    for (int i = 0; i < 7; i++) {
        if ((a.days >> i) & 1) {
            if (!s.empty()) s += " ";
            s += kDayLetters[i];
        }
    }
    return s;
}

std::string ClockApp::SourceText(const Alarm& a) const {
    if (a.src == 1) {
        const std::string n = Radio::GetInstance().StationName(a.station);
        return n.empty() ? "Radio" : "Radio: " + n;
    }
    if (a.src == 2) return std::string("MP3: ") + (a.file[0] ? a.file : "(ninguno)");
    return "Pitido";
}

void ClockApp::RefreshAlarmList() {
    char buf[160];
    for (int i = 0; i < kMaxAlarms; i++) {
        const Alarm& a = alarms_[i];
        snprintf(buf, sizeof(buf), "%02d:%02d   %s\n%s", a.hour, a.minute, DaysText(a).c_str(),
                 SourceText(a).c_str());
        SetLabel(row_lbl_[i], buf);
        lv_obj_set_style_bg_color(row_tog_[i], C(a.enabled ? 0x2ECC71 : 0x59627A), 0);
        SetButtonText(row_tog_[i], a.enabled ? "ON" : "OFF");
    }
    UpdateAlarmLabel();
}

void ClockApp::ScanMp3() {
    mp3_files_.clear();
    sd_note_.clear();
    if (!Radio::GetInstance().SdReady()) {
        sd_note_ = "Sin tarjeta SD";
        return;
    }
    const char* dirs[2] = {"/sdcard/alarmas", "/sdcard"};
    for (const char* dir : dirs) {
        DIR* d = opendir(dir);
        if (!d) continue;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr && mp3_files_.size() < 40) {
            if (e->d_name[0] == '.' || e->d_type == DT_DIR) continue;
            const std::string n = e->d_name;
            if (n.size() < 5 || n.size() > 47) continue;
            std::string ext = n.substr(n.size() - 4);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)tolower(c); });
            if (ext == ".mp3") mp3_files_.push_back(n);
        }
        closedir(d);
        if (!mp3_files_.empty()) {
            mp3_dir_ = dir;
            break;
        }
    }
    std::sort(mp3_files_.begin(), mp3_files_.end(),
              [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    if (mp3_files_.empty()) sd_note_ = "No hay MP3 en /sdcard/alarmas";
}

void ClockApp::OpenEditor(int idx) {
    edit_idx_ = idx;
    edit_ = alarms_[idx];
    test_playing_ = false;
    ScanMp3();
    RefreshEditor();
    Show(edit_panel_, true);
}

void ClockApp::RefreshEditor() {
    char buf[96];
    snprintf(buf, sizeof(buf), "Alarma %d", edit_idx_ + 1);
    SetLabel(ed_title_, buf);
    snprintf(buf, sizeof(buf), "%02d", edit_.hour);
    SetLabel(ed_hour_, buf);
    snprintf(buf, sizeof(buf), "%02d", edit_.minute);
    SetLabel(ed_min_, buf);
    for (int i = 0; i < 7; i++) {
        lv_obj_set_style_bg_color(ed_day_[i], C(((edit_.days >> i) & 1) ? kAccent : kPanel), 0);
    }
    const char* srcname = edit_.src == 1 ? "Radio" : edit_.src == 2 ? "MP3 de la SD" : "Pitido";
    snprintf(buf, sizeof(buf), "Sonido: %s  (toca para cambiar)", srcname);
    SetButtonText(ed_src_, buf);

    if (edit_.src == 1) {
        const std::string n = Radio::GetInstance().StationName(edit_.station);
        SetLabel(ed_sel_, n.empty() ? "(sin emisoras)" : n.c_str());
    } else if (edit_.src == 2) {
        if (edit_.file[0]) {
            SetLabel(ed_sel_, edit_.file);
        } else {
            SetLabel(ed_sel_, sd_note_.empty() ? "Elige un MP3 con < >" : sd_note_.c_str());
        }
    } else {
        SetLabel(ed_sel_, "El pitido de siempre");
    }
    SetButtonText(ed_test_, test_playing_ ? "PARAR" : "PROBAR");
}

// ---------------------------------------------------------------------------
// Botones
// ---------------------------------------------------------------------------

void ClockApp::HandleButton(int id) {
    Activity();

    // Aviso sonando
    if (id == 120) {
        Snooze();
        return;
    }
    if (id == 121) {
        StopRinging();
        return;
    }

    if (id == 1) {  // abrir las alarmas
        RefreshAlarmList();
        Show(alarm_panel_, true);
        return;
    }
    if (id == 80) {
        Show(alarm_panel_, false);
        return;
    }
    if (id >= 60 && id < 60 + kMaxAlarms) {  // activar o desactivar
        Alarm& a = alarms_[id - 60];
        a.enabled = a.enabled ? 0 : 1;
        SaveSettings();
        RefreshAlarmList();
        return;
    }
    if (id >= 70 && id < 70 + kMaxAlarms) {
        OpenEditor(id - 70);
        return;
    }

    // Editor
    if (id >= 90 && id <= 115) {
        if (id == 90) edit_.hour = (uint8_t)((edit_.hour + 23) % 24);
        else if (id == 91) edit_.hour = (uint8_t)((edit_.hour + 1) % 24);
        else if (id == 92) edit_.minute = (uint8_t)((edit_.minute + 55) % 60);
        else if (id == 93) edit_.minute = (uint8_t)((edit_.minute + 59) % 60);
        else if (id == 94) edit_.minute = (uint8_t)((edit_.minute + 1) % 60);
        else if (id == 95) edit_.minute = (uint8_t)((edit_.minute + 5) % 60);
        else if (id >= 100 && id <= 106) edit_.days ^= (uint8_t)(1 << (id - 100));
        else if (id == 110) edit_.src = (uint8_t)((edit_.src + 1) % 3);
        else if (id == 111 || id == 112) {
            const int dir = (id == 112) ? 1 : -1;
            if (edit_.src == 1) {
                const int n = Radio::GetInstance().StationCount();
                if (n > 0) edit_.station = (uint8_t)((edit_.station + dir + n) % n);
            } else if (edit_.src == 2 && !mp3_files_.empty()) {
                const int n = (int)mp3_files_.size();
                int cur = -1;
                for (int i = 0; i < n; i++) {
                    if (mp3_files_[i] == edit_.file) cur = i;
                }
                const int next = (cur < 0) ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
                snprintf(edit_.file, sizeof(edit_.file), "%s", mp3_files_[next].c_str());
            }
        } else if (id == 113) {  // probar el sonido
            if (test_playing_) {
                StopSound();
                test_playing_ = false;
            } else {
                use_beep_ = false;
                StartSound(edit_);
                if (edit_.src != 0) Radio::GetInstance().SetAlarmVolume(60);
                test_playing_ = true;
            }
        } else if (id == 114 || id == 115) {
            if (test_playing_) {
                StopSound();
                test_playing_ = false;
            }
            if (id == 114) {
                alarms_[edit_idx_] = edit_;
                alarms_[edit_idx_].enabled = 1;  // al guardar queda activada
                last_fire_key_[edit_idx_] = -1;
                SaveSettings();
                RefreshAlarmList();
            }
            Show(edit_panel_, false);
            return;
        }
        RefreshEditor();
    }
}

// ---------------------------------------------------------------------------
// Alarma
// ---------------------------------------------------------------------------

void ClockApp::CheckAlarms() {
    if (ringing_ || !time_ok_ || !TimeTrusted()) return;

    // Despues de "posponer"
    if (snooze_idx_ >= 0 && (int64_t)time(nullptr) >= snooze_until_) {
        const int i = snooze_idx_;
        snooze_idx_ = -1;
        StartRinging(i, true);
        return;
    }

    const int key = ((year_ * 13 + mon_) * 32 + mday_) * 1440 + h_ * 60 + m_;
    const int wd = (wday_ + 6) % 7;  // 0 = lunes
    for (int i = 0; i < kMaxAlarms; i++) {
        Alarm& a = alarms_[i];
        if (!a.enabled || a.hour != h_ || a.minute != m_) continue;
        if (a.days != 0 && !((a.days >> wd) & 1)) continue;
        if (last_fire_key_[i] == key) continue;
        last_fire_key_[i] = key;
        StartRinging(i, false);
        return;
    }
}

void ClockApp::StartRinging(int idx, bool from_snooze) {
    Alarm& a = alarms_[idx];
    ring_idx_ = idx;
    ringing_ = true;
    ring_secs_ = 0;
    sound_retries_ = 0;
    use_beep_ = false;
    SetDigit(ring_d_[0], a.hour / 10);
    SetDigit(ring_d_[1], a.hour % 10);
    SetDigit(ring_d_[2], a.minute / 10);
    SetDigit(ring_d_[3], a.minute % 10);
    const std::string txt = SourceText(a);
    SetLabel(ring_txt_, txt.c_str());
    Show(ring_panel_, true);
    Activity();
    StartSound(a);
    if (a.days == 0 && !from_snooze) {  // alarma de una sola vez: se desactiva al sonar
        a.enabled = 0;
        SaveSettings();
        RefreshAlarmList();
    }
    ESP_LOGI(TAG, "Despertador %d: %02d:%02d", idx + 1, a.hour, a.minute);
}

void ClockApp::StopRinging() {
    ringing_ = false;
    StopSound();
    Show(ring_panel_, false);
    UpdateAlarmLabel();
}

void ClockApp::Snooze() {
    if (ring_idx_ >= 0) {
        snooze_idx_ = ring_idx_;
        snooze_until_ = (int64_t)time(nullptr) + kSnoozeMin * 60;
    }
    StopRinging();
}

void ClockApp::RingTick() {
    ring_secs_++;
    lv_obj_set_style_bg_color(ring_panel_, C((ring_secs_ % 2) ? 0xB01E36 : 0x7A1020), 0);
    const Alarm& a = alarms_[ring_idx_ >= 0 ? ring_idx_ : 0];
    Radio& radio = Radio::GetInstance();

    if (a.src != 0 && !use_beep_) {
        radio.SetAlarmVolume(std::min(kMaxVol, kStartVol + ring_secs_ * 2));  // volumen creciente
        // Si el audio no arranco o termino (archivo corto), se repite; despues de 3 intentos, pitido
        if (ring_secs_ >= 4 && !radio.IsPlaying() && ring_secs_ % 5 == 0) {
            if (sound_retries_ < 3) {
                sound_retries_++;
                StartSound(a);
            } else {
                use_beep_ = true;
            }
        }
    }
    if ((a.src == 0 || use_beep_) && ring_secs_ % 2 == 1) Beep();
    if (ring_secs_ % 10 == 0) Activity();  // mantiene la pantalla encendida
    if (ring_secs_ >= kRingMaxSecs) StopRinging();  // si nadie la apaga, se para a los 10 minutos
}

void ClockApp::StartSound(const Alarm& a) {
    Radio& radio = Radio::GetInstance();
    if (a.src == 1) {
        const int n = radio.StationCount();
        if (n > 0) {
            const int i = std::min<int>(a.station, n - 1);
            radio.SetAlarmVolume(kStartVol);
            radio.PlayForAlarm(radio.StationUrl(i), radio.StationName(i), false);
            return;
        }
        use_beep_ = true;
        return;
    }
    if (a.src == 2) {
        const std::string path = ResolveMp3(a.file);
        if (!path.empty()) {
            radio.SetAlarmVolume(kStartVol);
            radio.PlayForAlarm(path, a.file, true);
            return;
        }
        use_beep_ = true;
        return;
    }
    Beep();
}

void ClockApp::StopSound() {
    Radio& radio = Radio::GetInstance();
    radio.StopForAlarm();
    radio.ClearAlarmVolume();
}

void ClockApp::Beep() {
#ifdef CLOCK_HAVE_SOUND
    Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
#endif
}
