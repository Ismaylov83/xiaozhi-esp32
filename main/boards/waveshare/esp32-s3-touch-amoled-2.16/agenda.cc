#include "agenda.h"
#include "display/display.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "Agenda"

namespace {

constexpr int kW = 480;
constexpr int kH = 480;
constexpr size_t kMaxEvents = 64;

constexpr uint32_t kBg = 0x0E1424;
constexpr uint32_t kPanel = 0x1A2238;
constexpr uint32_t kAccent = 0x3A8DDE;
constexpr uint32_t kOff = 0x1C2540;

// Cambia esta fuente si el texto se ve pequeno
const lv_font_t* kFont = LV_FONT_DEFAULT;

const char* kMonths[12] = {"Enero", "Febrero", "Marzo", "Abril", "Mayo", "Junio",
                           "Julio", "Agosto", "Septiembre", "Octubre", "Noviembre", "Diciembre"};
const char* kWeekdays[7] = {"Domingo", "Lunes", "Martes", "Miercoles", "Jueves", "Viernes", "Sabado"};
const char* kWdShort[7] = {"L", "M", "X", "J", "V", "S", "D"};

// Segmentos a b c d e f g (bit 0 a bit 6) de cada cifra
const uint8_t kSegMask[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

// Teclas del teclado propio (39 teclas, 10 por fila)
const char* const kKeys = "1234567890qwertyuiopasdfghjkl-zxcvbnm,.";

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
                    int y, int width = 0) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, C(color), 0);
    lv_obj_set_style_text_font(l, kFont, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (width > 0) lv_obj_set_width(l, width);
    lv_obj_align(l, align, x, y);
    return l;
}

// Panel que ocupa toda la pantalla y absorbe los toques
lv_obj_t* MakePanel(lv_obj_t* parent) {
    lv_obj_t* p = Box(parent, 0, 0, kW, kH, C(kBg), 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

void OnEvent(lv_event_t* e) {
    Agenda::GetInstance().HandleButton((int)(intptr_t)lv_event_get_user_data(e));
}

void OnTimer(lv_timer_t*) { Agenda::GetInstance().Tick(); }

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

// ---- Fechas ----

int DaysInMonth(int y, int m) {
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

// 0 = domingo ... 6 = sabado
int Weekday(int y, int m, int d) {
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// Columna del dia 1 con la semana empezando en lunes (0 = lunes)
int FirstWeekdayMon(int y, int m) { return (Weekday(y, m, 1) + 6) % 7; }

int EvKey(int y, int m, int d, int h, int mi) {
    return (((y * 12 + m) * 32 + d) * 24 + h) * 60 + mi;
}

// Mes y ano de compilacion: se usa mientras la placa aun no tiene hora
void BuildDate(int& y, int& m) {
    static const char* mn[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const char* d = __DATE__;  // "Nov 12 2026"
    m = 1;
    for (int i = 0; i < 12; i++) {
        if (strncmp(d, mn[i], 3) == 0) m = i + 1;
    }
    y = atoi(d + 7);
}

}  // namespace

Agenda& Agenda::GetInstance() {
    static Agenda instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Inicio, reloj y guardado
// ---------------------------------------------------------------------------

void Agenda::Init(Display* display) {
    if (root_ || !display) return;
    display_ = display;
    DisplayLockGuard lock(display_);
    LoadEvents();
    ReadClock();
    view_y_ = today_y_;
    view_m_ = today_m_;
    BuildUI();
    RefreshAll();
    lv_timer_create(OnTimer, 1000, nullptr);
    ESP_LOGI(TAG, "Agenda lista (%u citas)", (unsigned)events_.size());
}

void Agenda::Activity() {
    if (on_activity_) on_activity_();
}

void Agenda::ReadClock() {
    const time_t now = time(nullptr);
    if (now >= 1700000000) {
        struct tm t;
        localtime_r(&now, &t);
        time_ok_ = true;
        today_y_ = t.tm_year + 1900;
        today_m_ = t.tm_mon + 1;
        today_d_ = t.tm_mday;
        hour_ = t.tm_hour;
        min_ = t.tm_min;
    } else {
        time_ok_ = false;
        if (today_y_ == 0) BuildDate(today_y_, today_m_);
        today_d_ = 0;
        hour_ = -1;
        min_ = -1;
    }
}

int Agenda::NowKey() const { return EvKey(today_y_, today_m_, today_d_, hour_, min_); }

void Agenda::LoadEvents() {
    events_.clear();
    nvs_handle_t h;
    if (nvs_open("agenda", NVS_READONLY, &h) != ESP_OK) return;
    size_t size = 0;
    if (nvs_get_blob(h, "ev", nullptr, &size) == ESP_OK && size > 0 &&
        size % sizeof(Event) == 0 && size / sizeof(Event) <= kMaxEvents) {
        events_.resize(size / sizeof(Event));
        if (nvs_get_blob(h, "ev", events_.data(), &size) != ESP_OK) events_.clear();
    }
    nvs_close(h);
}

void Agenda::SaveEvents() {
    nvs_handle_t h;
    if (nvs_open("agenda", NVS_READWRITE, &h) != ESP_OK) return;
    if (events_.empty()) {
        nvs_erase_key(h, "ev");
    } else {
        nvs_set_blob(h, "ev", events_.data(), events_.size() * sizeof(Event));
    }
    nvs_commit(h);
    nvs_close(h);
}

bool Agenda::HasEvents(int y, int m, int d) const {
    for (const Event& e : events_) {
        if (e.year == y && e.month == m && e.day == d) return true;
    }
    return false;
}

void Agenda::SetTemperature(int celsius, bool valid) {
    temp_c_ = celsius;
    temp_ok_ = valid;
    if (visible_ && display_) {
        DisplayLockGuard lock(display_);
        RefreshClock();
    }
}

void Agenda::SetVisible(bool visible) {
    if (!display_ || !root_) return;
    DisplayLockGuard lock(display_);
    visible_ = visible;
    Show(root_, visible);
    if (visible) {
        ReadClock();
        view_y_ = today_y_;
        view_m_ = today_m_;
        sel_day_ = 0;
        CloseAllPanels();
        RefreshAll();
    }
    Activity();
}

void Agenda::CloseAllPanels() {
    Show(day_panel_, false);
    Show(edit_panel_, false);
    Show(up_panel_, false);
}

void Agenda::Tick() {
    if (!root_) return;
    const int oy = today_y_, om = today_m_, od = today_d_, oh = hour_, omin = min_;
    const bool ook = time_ok_;
    ReadClock();
    if (!visible_) return;
    if (time_ok_ && !ook) {  // acaba de llegar la hora
        view_y_ = today_y_;
        view_m_ = today_m_;
    }
    if (time_ok_ != ook || today_d_ != od || today_m_ != om || today_y_ != oy) {
        RefreshCalendar();
    }
    if (time_ok_ != ook || hour_ != oh || min_ != omin) {
        RefreshClock();
        UpdateNext();
    }
}

// ---------------------------------------------------------------------------
// Interfaz
// ---------------------------------------------------------------------------

void Agenda::MakeDigit(Digit& d, lv_obj_t* parent, int x, int y, int w, int h, int t,
                       uint32_t on) {
    d.on = on;
    const int uh = (h - 3 * t) / 2;
    const int hw = w - 2 * t;
    const lv_color_t off = C(kOff);
    d.seg[0] = Box(parent, x + t, y, hw, t, off, 2);                    // a
    d.seg[1] = Box(parent, x + w - t, y + t, t, uh, off, 2);            // b
    d.seg[2] = Box(parent, x + w - t, y + 2 * t + uh, t, uh, off, 2);   // c
    d.seg[3] = Box(parent, x + t, y + h - t, hw, t, off, 2);            // d
    d.seg[4] = Box(parent, x, y + 2 * t + uh, t, uh, off, 2);           // e
    d.seg[5] = Box(parent, x, y + t, t, uh, off, 2);                    // f
    d.seg[6] = Box(parent, x + t, y + t + uh, hw, t, off, 2);           // g
}

// v: 0-9 cifra, -1 guion, otro valor = apagado
void Agenda::SetDigit(Digit& d, int v) {
    uint8_t mask = 0;
    if (v >= 0 && v <= 9) {
        mask = kSegMask[v];
    } else if (v == -1) {
        mask = 0x40;
    }
    for (int i = 0; i < 7; i++) {
        lv_obj_set_style_bg_color(d.seg[i], C(((mask >> i) & 1) ? d.on : kOff), 0);
    }
}

void Agenda::BuildUI() {
    root_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, kW, kH);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, C(kBg), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    // --- Calendario del mes (izquierda) ---
    MakeButton(root_, 24, 18, 40, 32, kPanel, 8, "<", 1, 0xFFFFFF);
    MakeButton(root_, 236, 18, 40, 32, kPanel, 8, ">", 2, 0xFFFFFF);
    title_ = MakeLabel(root_, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 64, 26, 172);
    for (int c = 0; c < 7; c++) {
        MakeLabel(root_, kWdShort[c], c >= 5 ? 0xFF9A9A : 0x8FA3C8, LV_ALIGN_TOP_LEFT, 24 + c * 36,
                  54, 34);
    }
    for (int i = 0; i < 42; i++) {
        const int r = i / 7, c = i % 7;
        cell_[i] = Box(root_, 24 + c * 36, 74 + r * 36, 34, 32, C(kPanel), 8);
        lv_obj_add_flag(cell_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_color(cell_[i], C(0xFFD93D), 0);
        lv_obj_set_style_border_opa(cell_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell_[i], 0, 0);
        lv_obj_add_event_cb(cell_[i], OnEvent, LV_EVENT_CLICKED, (void*)(intptr_t)(100 + i));
        cell_lbl_[i] = lv_label_create(cell_[i]);
        lv_label_set_text(cell_lbl_[i], "");
        lv_obj_set_style_text_font(cell_lbl_[i], kFont, 0);
        lv_obj_center(cell_lbl_[i]);
        cell_dot_[i] = Circle(cell_[i], 14, 25, 5, C(0xFF6B6B));
        Show(cell_dot_[i], false);
    }

    // --- Hora grande (derecha) ---
    const int x0 = 290;
    MakeDigit(clk_[0], root_, x0, 60, 30, 56, 6, 0xFFFFFF);
    MakeDigit(clk_[1], root_, x0 + 36, 60, 30, 56, 6, 0xFFFFFF);
    colon_[0] = Circle(root_, x0 + 75, 76, 7, C(0xFFFFFF));
    colon_[1] = Circle(root_, x0 + 75, 94, 7, C(0xFFFFFF));
    MakeDigit(clk_[2], root_, x0 + 90, 60, 30, 56, 6, 0xFFFFFF);
    MakeDigit(clk_[3], root_, x0 + 126, 60, 30, 56, 6, 0xFFFFFF);

    // --- Temperatura (donde el ejemplo ponia el 36) ---
    MakeDigit(temp_[0], root_, 296, 140, 20, 36, 4, 0xFFD93D);
    MakeDigit(temp_[1], root_, 322, 140, 20, 36, 4, 0xFFD93D);
    MakeDigit(temp_[2], root_, 348, 140, 20, 36, 4, 0xFFD93D);
    lv_obj_t* deg = Circle(root_, 376, 142, 9, C(kBg));
    lv_obj_set_style_border_width(deg, 2, 0);
    lv_obj_set_style_border_color(deg, C(0xFFD93D), 0);
    lv_obj_set_style_border_opa(deg, LV_OPA_COVER, 0);
    MakeLabel(root_, "C", 0xFFD93D, LV_ALIGN_TOP_LEFT, 390, 148);

    // --- Fecha ---
    date_lbl_ = MakeLabel(root_, "", 0xDDE6FF, LV_ALIGN_TOP_LEFT, 290, 196, 170);

    // --- Boton redondo: recordatorios ---
    MakeButton(root_, 339, 262, 72, 72, 0xFF4D4D, LV_RADIUS_CIRCLE, "AVISOS", 3, 0xFFFFFF);

    next_lbl_ = MakeLabel(root_, "", 0xFFE066, LV_ALIGN_TOP_LEFT, 30, 352, 420);
    MakeButton(root_, 40, 396, 90, 40, kPanel, 12, "HOY", 4, 0xFFFFFF);

    // --- Panel del dia (agenda) ---
    day_panel_ = MakePanel(root_);
    day_title_ = MakeLabel(day_panel_, "", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 28, 380);
    for (int r = 0; r < kMaxRows; r++) {
        row_[r] = Box(day_panel_, 50, 70 + r * 52, 380, 46, C(kPanel), 10);
        row_lbl_[r] = MakeLabel(row_[r], "", 0xFFFFFF, LV_ALIGN_LEFT_MID, 12, 0, 300);
        lv_obj_set_style_text_align(row_lbl_[r], LV_TEXT_ALIGN_LEFT, 0);
        MakeButton(row_[r], 322, 6, 48, 34, 0xFF6B6B, 8, "X", 20 + r);
    }
    day_empty_ = MakeLabel(day_panel_, "Sin citas este dia", 0x8FA3C8, LV_ALIGN_TOP_MID, 0, 150, 380);
    MakeButton(day_panel_, 60, 410, 170, 48, 0x1DD1A1, 14, "+ CITA", 10);
    MakeButton(day_panel_, 250, 410, 170, 48, kPanel, 14, "VOLVER", 11, 0xFFFFFF);

    // --- Panel para apuntar una cita ---
    edit_panel_ = MakePanel(root_);
    edit_title_ = MakeLabel(edit_panel_, "", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 20, 380);
    MakeButton(edit_panel_, 50, 58, 44, 40, kPanel, 10, "-", 30, 0xFFFFFF);
    edit_h_lbl_ = MakeLabel(edit_panel_, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 98, 70, 54);
    MakeButton(edit_panel_, 156, 58, 44, 40, kPanel, 10, "+", 31, 0xFFFFFF);
    MakeButton(edit_panel_, 240, 58, 44, 40, kPanel, 10, "-", 32, 0xFFFFFF);
    edit_m_lbl_ = MakeLabel(edit_panel_, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 288, 70, 54);
    MakeButton(edit_panel_, 346, 58, 44, 40, kPanel, 10, "+", 33, 0xFFFFFF);
    edit_remind_btn_ = MakeButton(edit_panel_, 50, 106, 380, 36, 0xFF4D4D, 10, "", 34, 0xFFFFFF);

    // Campo de texto y teclado propios (no dependen de los widgets de LVGL)
    lv_obj_t* field = Box(edit_panel_, 50, 150, 380, 44, C(kPanel), 10);
    edit_txt_lbl_ = MakeLabel(field, "", 0xFFFFFF, LV_ALIGN_LEFT_MID, 12, 0, 356);
    lv_obj_set_style_text_align(edit_txt_lbl_, LV_TEXT_ALIGN_LEFT, 0);
    for (int i = 0; i < 39; i++) {
        const int r = i / 10, c = i % 10;
        const char s[2] = {kKeys[i], 0};
        MakeButton(edit_panel_, 50 + c * 38, 200 + r * 38, 36, 34, kPanel, 6, s, 200 + i, 0xFFFFFF);
    }
    MakeButton(edit_panel_, 50 + 9 * 38, 200 + 3 * 38, 36, 34, 0xFF6B6B, 6, "<", 290);
    MakeButton(edit_panel_, 50, 200 + 4 * 38, 380, 34, kPanel, 6, "ESPACIO", 291, 0xFFFFFF);

    MakeButton(edit_panel_, 60, 410, 170, 48, 0x1DD1A1, 14, "GUARDAR", 35);
    MakeButton(edit_panel_, 250, 410, 170, 48, kPanel, 14, "CANCELAR", 36, 0xFFFFFF);

    // --- Panel de recordatorios ---
    up_panel_ = MakePanel(root_);
    up_title_ = MakeLabel(up_panel_, "Proximos recordatorios", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 28, 380);
    for (int r = 0; r < kUpRows; r++) {
        up_lbl_[r] = MakeLabel(up_panel_, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 50, 72 + r * 44, 380);
        lv_obj_set_style_text_align(up_lbl_[r], LV_TEXT_ALIGN_LEFT, 0);
    }
    MakeButton(up_panel_, 140, 410, 200, 48, kPanel, 14, "VOLVER", 40, 0xFFFFFF);
}

void Agenda::RefreshAll() {
    RefreshCalendar();
    RefreshClock();
    UpdateNext();
}

void Agenda::RefreshCalendar() {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %d", kMonths[view_m_ - 1], view_y_);
    SetLabel(title_, buf);

    const int dim = DaysInMonth(view_y_, view_m_);
    const int off = FirstWeekdayMon(view_y_, view_m_);
    for (int i = 0; i < 42; i++) {
        const int day = i - off + 1;
        if (day < 1 || day > dim) {
            Show(cell_[i], false);
            continue;
        }
        Show(cell_[i], true);
        const bool is_today =
            time_ok_ && view_y_ == today_y_ && view_m_ == today_m_ && day == today_d_;
        lv_obj_set_style_bg_color(cell_[i], C(is_today ? kAccent : kPanel), 0);
        lv_obj_set_style_border_width(cell_[i], day == sel_day_ ? 2 : 0, 0);
        char d[8];
        snprintf(d, sizeof(d), "%d", day);
        SetLabel(cell_lbl_[i], d);
        lv_obj_set_style_text_color(cell_lbl_[i], C((i % 7) >= 5 ? 0xFF9A9A : 0xFFFFFF), 0);
        Show(cell_dot_[i], HasEvents(view_y_, view_m_, day));
    }
}

void Agenda::RefreshClock() {
    if (time_ok_) {
        SetDigit(clk_[0], hour_ / 10);
        SetDigit(clk_[1], hour_ % 10);
        SetDigit(clk_[2], min_ / 10);
        SetDigit(clk_[3], min_ % 10);
    } else {
        for (int i = 0; i < 4; i++) SetDigit(clk_[i], -1);
    }
    Show(colon_[0], true);
    Show(colon_[1], true);

    if (temp_ok_) {
        int a = temp_c_ < 0 ? -temp_c_ : temp_c_;
        if (a > 99) a = 99;
        SetDigit(temp_[0], temp_c_ < 0 ? -1 : -2);
        SetDigit(temp_[1], a >= 10 ? a / 10 : -2);
        SetDigit(temp_[2], a % 10);
    } else {
        SetDigit(temp_[0], -2);
        SetDigit(temp_[1], -1);
        SetDigit(temp_[2], -1);
    }

    char buf[64];
    if (time_ok_) {
        snprintf(buf, sizeof(buf), "%s\n%02d/%02d/%d",
                 kWeekdays[Weekday(today_y_, today_m_, today_d_)], today_d_, today_m_, today_y_);
    } else {
        snprintf(buf, sizeof(buf), "Sin hora\n(conecta al wifi)");
    }
    SetLabel(date_lbl_, buf);
}

void Agenda::UpdateNext() {
    char buf[96];
    const Event* next = nullptr;
    if (time_ok_) {
        const int now = NowKey();
        for (const Event& e : events_) {
            if (EvKey(e.year, e.month, e.day, e.hour, e.minute) >= now) {
                next = &e;
                break;
            }
        }
    }
    if (next) {
        snprintf(buf, sizeof(buf), "Proxima: %02d/%02d %02d:%02d  %s", next->day, next->month,
                 next->hour, next->minute, next->text);
    } else {
        snprintf(buf, sizeof(buf), "No hay citas proximas");
    }
    SetLabel(next_lbl_, buf);
}

// ---------------------------------------------------------------------------
// Agenda del dia, editor y recordatorios
// ---------------------------------------------------------------------------

void Agenda::OpenDay(int day) {
    sel_day_ = day;
    RefreshCalendar();
    RefreshDayPanel();
    Show(day_panel_, true);
}

void Agenda::RefreshDayPanel() {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s %d de %s", kWeekdays[Weekday(view_y_, view_m_, sel_day_)],
             sel_day_, kMonths[view_m_ - 1]);
    SetLabel(day_title_, buf);

    day_count_ = 0;
    for (size_t i = 0; i < events_.size() && day_count_ < kMaxRows; i++) {
        const Event& e = events_[i];
        if (e.year == view_y_ && e.month == view_m_ && e.day == sel_day_) {
            day_idx_[day_count_++] = (int)i;
        }
    }
    for (int r = 0; r < kMaxRows; r++) {
        if (r < day_count_) {
            const Event& e = events_[day_idx_[r]];
            snprintf(buf, sizeof(buf), "%02d:%02d  %s%s", e.hour, e.minute, e.remind ? "(!) " : "",
                     e.text);
            SetLabel(row_lbl_[r], buf);
            Show(row_[r], true);
        } else {
            Show(row_[r], false);
        }
    }
    Show(day_empty_, day_count_ == 0);
}

void Agenda::UpdateEditLabels() {
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d h", edit_h_);
    SetLabel(edit_h_lbl_, buf);
    snprintf(buf, sizeof(buf), "%02d min", edit_m_);
    SetLabel(edit_m_lbl_, buf);
    lv_obj_t* l = lv_obj_get_child(edit_remind_btn_, 0);
    if (l) SetLabel(l, edit_remind_ ? "Aviso sonoro: SI" : "Aviso sonoro: NO");
}

void Agenda::UpdateEditText() {
    if (edit_len_ == 0) {
        lv_obj_set_style_text_color(edit_txt_lbl_, C(0x6F7FA5), 0);
        SetLabel(edit_txt_lbl_, "Escribe el texto de la cita");
    } else {
        char buf[48];
        lv_obj_set_style_text_color(edit_txt_lbl_, C(0xFFFFFF), 0);
        snprintf(buf, sizeof(buf), "%s_", edit_text_);
        SetLabel(edit_txt_lbl_, buf);
    }
}

void Agenda::TypeKey(int id) {
    if (id == 290) {  // borrar
        if (edit_len_ > 0) edit_text_[--edit_len_] = 0;
    } else if (edit_len_ < 39) {
        char ch = 0;
        if (id == 291) {
            ch = ' ';
        } else if (id >= 200 && id < 200 + (int)strlen(kKeys)) {
            ch = kKeys[id - 200];
        }
        if (ch) {
            edit_text_[edit_len_++] = ch;
            edit_text_[edit_len_] = 0;
        }
    }
    UpdateEditText();
}

void Agenda::OpenEditor() {
    char buf[64];
    snprintf(buf, sizeof(buf), "Nueva cita - %02d/%02d/%d", sel_day_, view_m_, view_y_);
    SetLabel(edit_title_, buf);
    edit_h_ = (time_ok_ && hour_ >= 0) ? (hour_ + 1) % 24 : 9;
    edit_m_ = 0;
    edit_remind_ = true;
    edit_text_[0] = 0;
    edit_len_ = 0;
    UpdateEditText();
    UpdateEditLabels();
    Show(edit_panel_, true);
}

void Agenda::SaveEvent() {
    if (events_.size() >= kMaxEvents) {
        SetLabel(edit_title_, "Agenda llena (64 citas)");
        return;
    }
    Event e;
    memset(&e, 0, sizeof(e));
    e.year = (uint16_t)view_y_;
    e.month = (uint8_t)view_m_;
    e.day = (uint8_t)sel_day_;
    e.hour = (uint8_t)edit_h_;
    e.minute = (uint8_t)edit_m_;
    e.remind = edit_remind_ ? 1 : 0;
    const char* txt = edit_text_;
    snprintf(e.text, sizeof(e.text), "%s", (txt && txt[0]) ? txt : "Cita");
    events_.push_back(e);
    std::sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) {
        return EvKey(a.year, a.month, a.day, a.hour, a.minute) <
               EvKey(b.year, b.month, b.day, b.hour, b.minute);
    });
    SaveEvents();
    Show(edit_panel_, false);
    RefreshDayPanel();
    RefreshCalendar();
    UpdateNext();
}

void Agenda::DeleteEvent(int idx) {
    if (idx < 0 || idx >= (int)events_.size()) return;
    events_.erase(events_.begin() + idx);
    SaveEvents();
    RefreshDayPanel();
    RefreshCalendar();
    UpdateNext();
}

void Agenda::OpenUpcoming() {
    int n = 0;
    const int now = time_ok_ ? NowKey() : 0;
    char buf[96];
    for (const Event& e : events_) {
        if (n >= kUpRows) break;
        if (!e.remind) continue;
        if (time_ok_ && EvKey(e.year, e.month, e.day, e.hour, e.minute) < now) continue;
        snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d  %s", e.day, e.month, e.hour, e.minute,
                 e.text);
        SetLabel(up_lbl_[n++], buf);
    }
    for (int r = 0; r < kUpRows; r++) Show(up_lbl_[r], r < n);
    SetLabel(up_title_, n == 0 ? "No hay recordatorios" : "Proximos recordatorios");
    Show(up_panel_, true);
}

// ---------------------------------------------------------------------------
// Botones
// ---------------------------------------------------------------------------

void Agenda::HandleButton(int id) {
    Activity();

    if (id >= 200 && id < 300) {  // tecla del teclado
        TypeKey(id);
        return;
    }
    if (id >= 100 && id < 142) {  // un dia del calendario
        const int off = FirstWeekdayMon(view_y_, view_m_);
        const int day = (id - 100) - off + 1;
        if (day >= 1 && day <= DaysInMonth(view_y_, view_m_)) OpenDay(day);
        return;
    }
    if (id >= 20 && id < 20 + kMaxRows) {  // borrar cita del dia
        if (id - 20 < day_count_) DeleteEvent(day_idx_[id - 20]);
        return;
    }

    switch (id) {
        case 1:
            if (--view_m_ < 1) {
                view_m_ = 12;
                view_y_--;
            }
            sel_day_ = 0;
            RefreshCalendar();
            break;
        case 2:
            if (++view_m_ > 12) {
                view_m_ = 1;
                view_y_++;
            }
            sel_day_ = 0;
            RefreshCalendar();
            break;
        case 3:
            OpenUpcoming();
            break;
        case 4:
            view_y_ = today_y_;
            view_m_ = today_m_;
            sel_day_ = 0;
            RefreshCalendar();
            break;
        case 10:
            OpenEditor();
            break;
        case 11:
            Show(day_panel_, false);
            sel_day_ = 0;
            RefreshCalendar();
            break;
        case 30:
            edit_h_ = (edit_h_ + 23) % 24;
            UpdateEditLabels();
            break;
        case 31:
            edit_h_ = (edit_h_ + 1) % 24;
            UpdateEditLabels();
            break;
        case 32:
            edit_m_ = (edit_m_ + 55) % 60;
            UpdateEditLabels();
            break;
        case 33:
            edit_m_ = (edit_m_ + 5) % 60;
            UpdateEditLabels();
            break;
        case 34:
            edit_remind_ = !edit_remind_;
            UpdateEditLabels();
            break;
        case 35:
            SaveEvent();
            break;
        case 36:
            Show(edit_panel_, false);
            break;
        case 40:
            Show(up_panel_, false);
            break;
        default:
            break;
    }
}
