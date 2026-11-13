#include "agenda.h"
#include "application.h"
#include "display/display.h"
#include "mcp_server.h"

#if __has_include("assets/lang_config.h")
#include "assets/lang_config.h"
#define AGENDA_HAVE_SOUND 1
#endif

#include <esp_log.h>
#include <esp_wifi.h>
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
constexpr int kOy = 14;  // desplazamiento vertical del calendario y la columna derecha

constexpr uint32_t kBg = 0x0E1424;
constexpr uint32_t kPanel = 0x1A2238;
constexpr uint32_t kAccent = 0x3A8DDE;
constexpr uint32_t kOff = 0x1C2540;

// Cambia esta fuente si el texto se ve pequeno
const lv_font_t* kFont = LV_FONT_DEFAULT;

const char* kMonths[12] = {"Enero", "Febrero", "Marzo", "Abril", "Mayo", "Junio",
                           "Julio", "Agosto", "Septiembre", "Octubre", "Noviembre", "Diciembre"};
const char* kWeekdays[7] = {"Domingo", "Lunes", "Martes", "Miercoles", "Jueves", "Viernes", "Sabado"};
const char* kWdAbbr[7] = {"Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab"};
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
    {
        DisplayLockGuard lock(display_);
        LoadEvents();
        ReadClock();
        view_y_ = today_y_;
        view_m_ = today_m_;
        BuildUI();
        RefreshAll();
        lv_timer_create(OnTimer, 1000, nullptr);
    }
    RegisterTools();
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

void Agenda::SortEvents() {
    std::sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) {
        return EvKey(a.year, a.month, a.day, a.hour, a.minute) <
               EvKey(b.year, b.month, b.day, b.hour, b.minute);
    });
}

bool Agenda::HasEvents(int y, int m, int d) const {
    for (const Event& e : events_) {
        if (e.year == y && e.month == m && e.day == d) return true;
    }
    return false;
}

void Agenda::SetTemperature(int celsius, bool valid) {
    if (!display_) return;
    DisplayLockGuard lock(display_);
    temp_c_ = celsius;
    temp_ok_ = valid;
    if (visible_) RefreshClock();
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
    Show(fc_panel_, false);
}

void Agenda::Tick() {
    if (!root_) return;
    tick_count_++;
    const int oy = today_y_, om = today_m_, od = today_d_, oh = hour_, omin = min_;
    const bool ook = time_ok_;
    ReadClock();

    // Los avisos funcionan aunque la agenda no este en pantalla
    if (time_ok_) CheckAlarms();
    AlarmTick();

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
    if (tick_count_ % 5 == 0) UpdateStatusIcons();
}

// ---------------------------------------------------------------------------
// Avisos (alarma con sonido)
// ---------------------------------------------------------------------------

void Agenda::CheckAlarms() {
    if (alarm_active_) return;
    const int now = NowKey();
    bool changed = false;
    for (Event& e : events_) {
        if (e.fired) continue;
        const int key = EvKey(e.year, e.month, e.day, e.hour, e.minute);
        if (key > now) continue;
        e.fired = 1;
        changed = true;
        // Si la hora ya paso hace mas de 10 minutos no suena (cita antigua)
        if (e.remind && now - key <= 10) {
            StartAlarm(e);
            break;
        }
    }
    if (changed) {
        SaveEvents();
        UpdateAvisos();
        if (visible_) {
            RefreshCalendar();
            UpdateNext();
        }
    }
}

void Agenda::StartAlarm(const Event& e) {
    SetDigit(alarm_clk_[0], e.hour / 10);
    SetDigit(alarm_clk_[1], e.hour % 10);
    SetDigit(alarm_clk_[2], e.minute / 10);
    SetDigit(alarm_clk_[3], e.minute % 10);
    SetLabel(alarm_txt_, e.text);
    alarm_active_ = true;
    alarm_secs_ = 0;
    Show(alarm_panel_, true);
    Activity();
    ESP_LOGI(TAG, "Aviso: %02d:%02d %s", e.hour, e.minute, e.text);
}

void Agenda::DismissAlarm() {
    alarm_active_ = false;
    Show(alarm_panel_, false);
    UpdateAvisos();
}

void Agenda::AlarmTick() {
    if (!alarm_active_) return;
    alarm_secs_++;
    lv_obj_set_style_bg_color(alarm_panel_, C((alarm_secs_ % 2) ? 0xB01E36 : 0x7A1020), 0);
    if (alarm_secs_ % 2 == 1) Beep();
    if (alarm_secs_ % 10 == 0) Activity();  // mantiene la pantalla encendida
    if (alarm_secs_ >= 90) DismissAlarm();   // si nadie lo atiende, se apaga solo
}

void Agenda::Beep() {
#ifdef AGENDA_HAVE_SOUND
    Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
#endif
}

// Verde: sin avisos pendientes. Rojo: hay algun aviso por sonar.
void Agenda::UpdateAvisos() {
    bool pending = false;
    for (const Event& e : events_) {
        if (e.remind && !e.fired) {
            pending = true;
            break;
        }
    }
    pending_reminders_.store(pending);
    if (avisos_btn_) lv_obj_set_style_bg_color(avisos_btn_, C(pending ? 0xE53935 : 0x2ECC71), 0);
}

void Agenda::UpdateStatusIcons() {
    // WiFi
    int bars = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        const int r = ap.rssi;
        bars = r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
    }
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(wifi_bar_[i], C(i < bars ? 0xFFFFFF : kOff), 0);
    }

    // Bateria
    int level = 0;
    bool charging = false;
    char buf[16];
    if (battery_cb_ && battery_cb_(level, charging)) {
        level = std::max(0, std::min(100, level));
        lv_obj_set_width(bat_fill_, std::max(2, (32 * level) / 100));
        const uint32_t col = charging ? 0x4DA3FF : level <= 15 ? 0xFF4D4D : level <= 35 ? 0xFFB020 : 0x3DDC84;
        lv_obj_set_style_bg_color(bat_fill_, C(col), 0);
        snprintf(buf, sizeof(buf), "%d%%", level);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    SetLabel(bat_lbl_, buf);
}

// ---------------------------------------------------------------------------
// Herramientas de voz (MCP) para xiaozhi
// ---------------------------------------------------------------------------

void Agenda::RefreshAfterChange() {
    RefreshCalendar();
    UpdateNext();
    UpdateAvisos();
    if (day_panel_ && !lv_obj_has_flag(day_panel_, LV_OBJ_FLAG_HIDDEN)) RefreshDayPanel();
}

std::string Agenda::AddEventApi(int y, int m, int d, int h, int mi, const std::string& text,
                                bool remind) {
    if (y < 2020 || y > 2100 || m < 1 || m > 12 || d < 1 || d > DaysInMonth(y, m) || h < 0 ||
        h > 23 || mi < 0 || mi > 59) {
        return "ERROR: invalid date or time";
    }
    DisplayLockGuard lock(display_);
    if (events_.size() >= kMaxEvents) return "ERROR: the agenda is full (64 events)";

    Event e;
    memset(&e, 0, sizeof(e));
    e.year = (uint16_t)y;
    e.month = (uint8_t)m;
    e.day = (uint8_t)d;
    e.hour = (uint8_t)h;
    e.minute = (uint8_t)mi;
    e.remind = remind ? 1 : 0;
    size_t n = std::min<size_t>(text.size(), sizeof(e.text) - 1);
    while (n > 0 && n < text.size() && ((uint8_t)text[n] & 0xC0) == 0x80) n--;  // no cortar un caracter UTF-8
    memcpy(e.text, text.data(), n);
    if (e.text[0] == 0) snprintf(e.text, sizeof(e.text), "Cita");

    const bool past = time_ok_ && EvKey(y, m, d, h, mi) <= NowKey();
    e.fired = past ? 1 : 0;  // una cita en el pasado no suena

    events_.push_back(e);
    SortEvents();
    SaveEvents();
    RefreshAfterChange();

    char buf[128];
    snprintf(buf, sizeof(buf), "OK: added %04d-%02d-%02d %02d:%02d \"%s\"%s%s", y, m, d, h, mi,
             e.text, remind ? " with sound reminder" : "", past ? " (this time is already in the past, it will not ring)" : "");
    return buf;
}

std::string Agenda::ListEventsApi(int y, int m, int d) {
    DisplayLockGuard lock(display_);
    std::string out;
    int n = 0;
    char buf[96];
    for (const Event& e : events_) {
        if (e.year != y || e.month != m) continue;
        if (d != 0 && e.day != d) continue;
        if (n >= 20) break;
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d %s%s\n", e.year, e.month, e.day,
                 e.hour, e.minute, e.text, e.remind ? " (reminder)" : "");
        out += buf;
        n++;
    }
    if (n == 0) return "No events found for that date";
    return out;
}

std::string Agenda::DeleteEventApi(int y, int m, int d, int h, int mi) {
    DisplayLockGuard lock(display_);
    for (size_t i = 0; i < events_.size(); i++) {
        const Event& e = events_[i];
        if (e.year == y && e.month == m && e.day == d && e.hour == h && e.minute == mi) {
            char buf[96];
            snprintf(buf, sizeof(buf), "OK: deleted \"%s\"", e.text);
            events_.erase(events_.begin() + i);
            SaveEvents();
            RefreshAfterChange();
            return buf;
        }
    }
    return "ERROR: no event found at that exact date and time";
}

void Agenda::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.agenda.get_date_time",
                "Get the current local date, time and weekday of the device. "
                "Call this first to work out words such as today, tomorrow or next Friday "
                "before adding, reading or deleting agenda events.",
                PropertyList(), [](const PropertyList&) -> ReturnValue {
                    const time_t now = time(nullptr);
                    if (now < 1700000000) return std::string("The device does not have the time yet");
                    struct tm t;
                    localtime_r(&now, &t);
                    char buf[96];
                    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d (%s)", t.tm_year + 1900,
                             t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, kWeekdays[t.tm_wday]);
                    return std::string(buf);
                });

    mcp.AddTool("self.agenda.add_event",
                "Add an appointment or event to the agenda (calendar) of this device. "
                "Use 24-hour time. If remind is true the device rings an alarm at that time. "
                "Call self.agenda.get_date_time first to work out the exact date.",
                PropertyList({Property("year", kPropertyTypeInteger, 2020, 2100),
                              Property("month", kPropertyTypeInteger, 1, 12),
                              Property("day", kPropertyTypeInteger, 1, 31),
                              Property("hour", kPropertyTypeInteger, 0, 23),
                              Property("minute", kPropertyTypeInteger, 0, 59),
                              Property("text", kPropertyTypeString),
                              Property("remind", kPropertyTypeBoolean, true)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    return AddEventApi(properties["year"].value<int>(),
                                       properties["month"].value<int>(),
                                       properties["day"].value<int>(),
                                       properties["hour"].value<int>(),
                                       properties["minute"].value<int>(),
                                       properties["text"].value<std::string>(),
                                       properties["remind"].value<bool>());
                });

    mcp.AddTool("self.agenda.list_events",
                "List the events of the agenda for a given day, or for a whole month if day is 0. "
                "Call self.agenda.get_date_time first to work out the exact date.",
                PropertyList({Property("year", kPropertyTypeInteger, 2020, 2100),
                              Property("month", kPropertyTypeInteger, 1, 12),
                              Property("day", kPropertyTypeInteger, 0, 0, 31)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    return ListEventsApi(properties["year"].value<int>(),
                                         properties["month"].value<int>(),
                                         properties["day"].value<int>());
                });

    mcp.AddTool("self.agenda.delete_event",
                "Delete an agenda event. The exact date and time of the event are required; "
                "list the events first if you do not know them.",
                PropertyList({Property("year", kPropertyTypeInteger, 2020, 2100),
                              Property("month", kPropertyTypeInteger, 1, 12),
                              Property("day", kPropertyTypeInteger, 1, 31),
                              Property("hour", kPropertyTypeInteger, 0, 23),
                              Property("minute", kPropertyTypeInteger, 0, 59)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    return DeleteEventApi(properties["year"].value<int>(),
                                          properties["month"].value<int>(),
                                          properties["day"].value<int>(),
                                          properties["hour"].value<int>(),
                                          properties["minute"].value<int>());
                });
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

    // --- Fila superior: wifi (izquierda), hora (centro), bateria (derecha) ---
    for (int i = 0; i < 4; i++) {
        const int h = 7 + i * 5;
        wifi_bar_[i] = Box(root_, 60 + i * 8, 44 - h, 5, h, C(kOff), 1);
    }
    lv_obj_t* bat = Box(root_, 392, 26, 38, 18, C(kBg), 3);
    lv_obj_set_style_border_width(bat, 2, 0);
    lv_obj_set_style_border_color(bat, C(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(bat, LV_OPA_COVER, 0);
    bat_fill_ = Box(root_, 395, 29, 32, 12, C(0x3DDC84), 2);
    Box(root_, 430, 31, 3, 8, C(0xFFFFFF), 1);
    bat_lbl_ = MakeLabel(root_, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 338, 27, 50);
    lv_obj_set_style_text_align(bat_lbl_, LV_TEXT_ALIGN_RIGHT, 0);

    const int x0 = 162;
    const int cy = 18;
    MakeDigit(clk_[0], root_, x0, cy, 30, 56, 6, 0xFFFFFF);
    MakeDigit(clk_[1], root_, x0 + 36, cy, 30, 56, 6, 0xFFFFFF);
    colon_[0] = Circle(root_, x0 + 75, cy + 16, 7, C(0xFFFFFF));
    colon_[1] = Circle(root_, x0 + 75, cy + 34, 7, C(0xFFFFFF));
    MakeDigit(clk_[2], root_, x0 + 90, cy, 30, 56, 6, 0xFFFFFF);
    MakeDigit(clk_[3], root_, x0 + 126, cy, 30, 56, 6, 0xFFFFFF);

    // --- Calendario del mes (izquierda) ---
    MakeButton(root_, 20, 84 + kOy, 40, 30, kPanel, 8, "<", 1, 0xFFFFFF);
    MakeButton(root_, 267, 84 + kOy, 40, 30, kPanel, 8, ">", 2, 0xFFFFFF);
    title_ = MakeLabel(root_, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 62, 92 + kOy, 203);
    for (int c = 0; c < 7; c++) {
        MakeLabel(root_, kWdShort[c], c >= 5 ? 0xFF9A9A : 0x8FA3C8, LV_ALIGN_TOP_LEFT, 20 + c * 41,
                  118 + kOy, 39);
    }
    for (int i = 0; i < 42; i++) {
        const int r = i / 7, c = i % 7;
        cell_[i] = Box(root_, 20 + c * 41, 138 + kOy + r * 37, 39, 34, C(kPanel), 8);
        lv_obj_add_flag(cell_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_color(cell_[i], C(0xFFD93D), 0);
        lv_obj_set_style_border_opa(cell_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell_[i], 0, 0);
        lv_obj_add_event_cb(cell_[i], OnEvent, LV_EVENT_CLICKED, (void*)(intptr_t)(100 + i));
        cell_lbl_[i] = lv_label_create(cell_[i]);
        lv_label_set_text(cell_lbl_[i], "");
        lv_obj_set_style_text_font(cell_lbl_[i], kFont, 0);
        lv_obj_center(cell_lbl_[i]);
        cell_dot_[i] = Circle(cell_[i], 17, 26, 5, C(0xFF6B6B));
        Show(cell_dot_[i], false);
    }

    // --- Columna derecha: clima, fecha y avisos ---
    // Zona tactil del clima (abre la prevision de la semana)
    lv_obj_t* clima = Box(root_, 322, 100 + kOy, 116, 106, C(kBg), 12);
    lv_obj_set_style_bg_opa(clima, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(clima, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clima, OnEvent, LV_EVENT_CLICKED, (void*)(intptr_t)5);

    MakeDigit(temp_[0], root_, 330, 100 + kOy, 18, 32, 4, 0xFFD93D);
    MakeDigit(temp_[1], root_, 352, 100 + kOy, 18, 32, 4, 0xFFD93D);
    MakeDigit(temp_[2], root_, 374, 100 + kOy, 18, 32, 4, 0xFFD93D);
    lv_obj_t* deg = Circle(root_, 398, 102 + kOy, 8, C(kBg));
    lv_obj_set_style_border_width(deg, 2, 0);
    lv_obj_set_style_border_color(deg, C(0xFFD93D), 0);
    lv_obj_set_style_border_opa(deg, LV_OPA_COVER, 0);
    MakeLabel(root_, "C", 0xFFD93D, LV_ALIGN_TOP_LEFT, 410, 107 + kOy);

    // Nube (icono del clima; en la siguiente etapa cambiara segun el tiempo)
    const lv_color_t cloud = C(0x8FA3C8);
    Circle(root_, 340, 150 + kOy + 18, 24, cloud);
    Circle(root_, 356, 150 + kOy + 6, 32, cloud);
    Circle(root_, 382, 150 + kOy + 16, 26, cloud);
    Box(root_, 352, 150 + kOy + 28, 52, 16, cloud, 8);

    date_lbl_ = MakeLabel(root_, "", 0xDDE6FF, LV_ALIGN_TOP_LEFT, 322, 206 + kOy, 116);

    avisos_btn_ = MakeButton(root_, 348, 268 + kOy, 64, 64, 0x2ECC71, LV_RADIUS_CIRCLE, "AVISOS", 3,
                             0xFFFFFF);

    // --- Parte baja: cita pendiente (izquierda) y HOY (derecha) ---
    next_lbl_ = MakeLabel(root_, "", 0xFFE066, LV_ALIGN_TOP_LEFT, 20, 372 + kOy, 287);
    MakeButton(root_, 330, 372 + kOy, 100, 38, kPanel, 12, "HOY", 4, 0xFFFFFF);

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

    // --- Panel de la prevision de la semana (aun sin datos) ---
    fc_panel_ = MakePanel(root_);
    MakeLabel(fc_panel_, "Prevision de la semana", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 28, 380);
    MakeLabel(fc_panel_, "Todavia no hay datos del clima.\nSe activara en la siguiente etapa.",
              0x8FA3C8, LV_ALIGN_TOP_MID, 0, 180, 380);
    MakeButton(fc_panel_, 140, 410, 200, 48, kPanel, 14, "VOLVER", 41, 0xFFFFFF);

    // --- Aviso a pantalla completa (encima de todo, tambien en otros modos) ---
    alarm_panel_ = Box(lv_layer_top(), 0, 0, kW, kH, C(0x7A1020), 0);
    lv_obj_add_flag(alarm_panel_, LV_OBJ_FLAG_CLICKABLE);
    Show(alarm_panel_, false);
    MakeLabel(alarm_panel_, "AVISO DE LA AGENDA", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 70, 380);
    const int ax = 128;
    MakeDigit(alarm_clk_[0], alarm_panel_, ax, 110, 44, 80, 9, 0xFFFFFF);
    MakeDigit(alarm_clk_[1], alarm_panel_, ax + 52, 110, 44, 80, 9, 0xFFFFFF);
    Circle(alarm_panel_, ax + 106, 110 + 24, 12, C(0xFFFFFF));
    Circle(alarm_panel_, ax + 106, 110 + 50, 12, C(0xFFFFFF));
    MakeDigit(alarm_clk_[2], alarm_panel_, ax + 128, 110, 44, 80, 9, 0xFFFFFF);
    MakeDigit(alarm_clk_[3], alarm_panel_, ax + 180, 110, 44, 80, 9, 0xFFFFFF);
    alarm_txt_ = MakeLabel(alarm_panel_, "", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 226, 380);
    MakeButton(alarm_panel_, 140, 320, 200, 64, 0xFFFFFF, 20, "ENTENDIDO", 50, 0x7A1020);
}

void Agenda::RefreshAll() {
    RefreshCalendar();
    RefreshClock();
    UpdateNext();
    UpdateAvisos();
    UpdateStatusIcons();
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
        snprintf(buf, sizeof(buf), "%s %02d/%02d\n%d",
                 kWdAbbr[Weekday(today_y_, today_m_, today_d_)], today_d_, today_m_, today_y_);
    } else {
        snprintf(buf, sizeof(buf), "Sin hora");
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
        snprintf(buf, sizeof(buf), "Cita pendiente: %02d/%02d %02d:%02d  %s", next->day,
                 next->month, next->hour, next->minute, next->text);
    } else {
        snprintf(buf, sizeof(buf), "Sin citas pendientes");
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
    snprintf(e.text, sizeof(e.text), "%s", edit_text_[0] ? edit_text_ : "Cita");
    // Una cita en el pasado no debe sonar
    e.fired = (time_ok_ && EvKey(view_y_, view_m_, sel_day_, edit_h_, edit_m_) <= NowKey()) ? 1 : 0;
    events_.push_back(e);
    SortEvents();
    SaveEvents();
    Show(edit_panel_, false);
    RefreshAfterChange();
}

void Agenda::DeleteEvent(int idx) {
    if (idx < 0 || idx >= (int)events_.size()) return;
    events_.erase(events_.begin() + idx);
    SaveEvents();
    RefreshAfterChange();
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

    if (id == 50) {  // ENTENDIDO en el aviso
        DismissAlarm();
        return;
    }
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
        case 5:
            Show(fc_panel_, true);
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
        case 41:
            Show(fc_panel_, false);
            break;
        default:
            break;
    }
}
