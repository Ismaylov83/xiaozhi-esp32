#include "radio.h"
#include "display/display.h"

#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdmmc_cmd.h>

#include <algorithm>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define TAG "Radio"

namespace {

constexpr int kW = 480;
constexpr int kH = 480;

constexpr uint32_t kBg = 0x0A0F1E;
constexpr uint32_t kPanel = 0x1A2238;
constexpr uint32_t kListBg = 0x131B33;
constexpr uint32_t kSelected = 0x2C4A6B;
constexpr uint32_t kOff = 0x1C2540;

// Tarjeta SD de la placa (por SPI, segun el esquema de Waveshare)
constexpr gpio_num_t kSdMosi = GPIO_NUM_1;
constexpr gpio_num_t kSdSck = GPIO_NUM_2;
constexpr gpio_num_t kSdMiso = GPIO_NUM_3;
constexpr gpio_num_t kSdCs = GPIO_NUM_41;
const char* const kSdBase = "/sdcard";

// Cambia esta fuente si el texto se ve pequeno
const lv_font_t* kFont = LV_FONT_DEFAULT;

const uint8_t kSegMask[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

struct DefaultStation {
    const char* name;
    const char* genre;
    const char* url;
};

// Emisoras de serie (MP3 por http). Se pueden anadir mas desde la SD en la siguiente etapa.
const DefaultStation kDefaultStations[] = {
    {"Groove Salad", "Ambient", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"Drone Zone", "Ambient", "http://ice1.somafm.com/dronezone-128-mp3"},
    {"Secret Agent", "Lounge", "http://ice1.somafm.com/secretagent-128-mp3"},
    {"Lush", "Chill", "http://ice1.somafm.com/lush-128-mp3"},
    {"Space Station", "Electronica", "http://ice1.somafm.com/spacestation-128-mp3"},
    {"Indie Pop Rocks", "Indie", "http://ice1.somafm.com/indiepop-128-mp3"},
    {"Defcon Radio", "Hacker", "http://ice1.somafm.com/defcon-128-mp3"},
    {"Radio Paradise", "Eclectic", "http://stream.radioparadise.com/mp3-128"},
};

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
    Radio::GetInstance().HandleButton((int)(intptr_t)lv_event_get_user_data(e));
}

void OnTimer(lv_timer_t*) { Radio::GetInstance().Tick(); }

void OnAsyncSd(void*) { Radio::GetInstance().RebuildSdList(); }

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

// Fila pulsable dentro de una lista con scroll
lv_obj_t* MakeRow(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int id) {
    lv_obj_t* r = Box(parent, x, y, w, h, C(color), 10);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, OnEvent, LV_EVENT_CLICKED, (void*)(intptr_t)id);
    return r;
}

// Lista que se desplaza con el dedo
void MakeScrollable(lv_obj_t* o) {
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
}

// Tipo de archivo por la extension: 'A' audio, 'V' video, ' ' otro
char FileKind(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return ' ';
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)tolower(c); });
    if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "aac" || ext == "ogg" ||
        ext == "m4a")
        return 'A';
    if (ext == "avi" || ext == "mp4" || ext == "mjpg" || ext == "mkv") return 'V';
    return ' ';
}

}  // namespace

Radio& Radio::GetInstance() {
    static Radio instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Inicio y ajustes
// ---------------------------------------------------------------------------

void Radio::Init(Display* display) {
    if (root_ || !display) return;
    display_ = display;
    DisplayLockGuard lock(display_);
    LoadSettings();
    LoadStations();
    BuildUI();
    RefreshAll();
    lv_timer_create(OnTimer, 1000, nullptr);
    ESP_LOGI(TAG, "Radio lista (%u emisoras)", (unsigned)stations_.size());
}

void Radio::Activity() {
    if (on_activity_) on_activity_();
}

void Radio::LoadSettings() {
    nvs_handle_t h;
    if (nvs_open("radio", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    if (nvs_get_i32(h, "vol", &v) == ESP_OK) volume_ = std::max(0, std::min(100, (int)v));
    if (nvs_get_i32(h, "sel", &v) == ESP_OK) selected_ = (int)v;
    nvs_close(h);
}

void Radio::SaveSettings() {
    nvs_handle_t h;
    if (nvs_open("radio", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "vol", volume_);
    nvs_set_i32(h, "sel", selected_);
    nvs_commit(h);
    nvs_close(h);
}

void Radio::LoadStations() {
    stations_.clear();
    for (const DefaultStation& s : kDefaultStations) {
        stations_.push_back({s.name, s.genre, s.url});
    }
    if (selected_ < 0 || selected_ >= (int)stations_.size()) selected_ = 0;
}

void Radio::SetVisible(bool visible) {
    if (!display_ || !root_) return;
    DisplayLockGuard lock(display_);
    visible_ = visible;
    Show(root_, visible);
    if (visible) {
        Show(sd_panel_, false);
        RefreshAll();
    }
    Activity();
}

void Radio::Tick() {
    if (!root_ || !visible_) return;
    tick_count_++;
    RefreshClock();
    if (tick_count_ % 5 == 0) UpdateStatusIcons();
}

// ---------------------------------------------------------------------------
// Tarjeta SD
// ---------------------------------------------------------------------------

bool Radio::MountSd() {
    if (sd_mounted_) return true;

    spi_bus_config_t bus = {};
    bus.mosi_io_num = kSdMosi;
    bus.miso_io_num = kSdMiso;
    bus.sclk_io_num = kSdSck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize fallo: %s", esp_err_to_name(ret));
        sd_msg_ = "Error al iniciar el bus de la SD";
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = kSdCs;
    slot.host_id = SPI3_HOST;

    esp_vfs_fat_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 4;
    mount_cfg.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    ret = esp_vfs_fat_sdspi_mount(kSdBase, &host, &slot, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar la SD: %s", esp_err_to_name(ret));
        sd_msg_ = (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_TIMEOUT)
                      ? "No hay tarjeta SD"
                      : "No se pudo leer la SD (formato FAT32?)";
        return false;
    }
    sd_card_ = card;
    sd_mounted_ = true;
    ESP_LOGI(TAG, "SD montada: %.8s", card->cid.name);
    return true;
}

void Radio::ReadDir() {
    sd_entries_.clear();
    if (sd_path_ != kSdBase) sd_entries_.push_back({"..", true});

    DIR* d = opendir(sd_path_.c_str());
    if (!d) {
        sd_msg_ = "No se pudo abrir la carpeta";
        return;
    }
    std::vector<SdEntry> items;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && (int)items.size() < kMaxSdEntries) {
        if (e->d_name[0] == '.') continue;
        items.push_back({e->d_name, e->d_type == DT_DIR});
    }
    closedir(d);

    // Carpetas primero y despues por orden alfabetico
    std::sort(items.begin(), items.end(), [](const SdEntry& a, const SdEntry& b) {
        if (a.dir != b.dir) return a.dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    for (const SdEntry& it : items) sd_entries_.push_back(it);
    sd_msg_ = items.empty() ? "Carpeta vacia" : "";
}

void Radio::OpenSd() {
    sd_path_ = kSdBase;
    sd_msg_ = "";
    if (MountSd()) {
        ReadDir();
    } else {
        sd_entries_.clear();
    }
    RebuildSdList();
    Show(sd_panel_, true);
}

// Redibuja la lista de archivos de la carpeta actual
void Radio::RebuildSdList() {
    if (!sd_list_) return;
    lv_obj_clean(sd_list_);

    SetLabel(sd_title_, sd_path_.empty() ? "Tarjeta SD" : sd_path_.c_str());
    SetLabel(sd_info_, sd_msg_.c_str());

    for (size_t i = 0; i < sd_entries_.size(); i++) {
        const SdEntry& en = sd_entries_[i];
        lv_obj_t* r = MakeRow(sd_list_, 0, (int)i * 42, 410, 38, kPanel, 1000 + (int)i);
        const char kind = en.dir ? 'D' : FileKind(en.name);
        const char* prefix = en.dir ? "> " : kind == 'A' ? "* " : kind == 'V' ? "V " : "  ";
        std::string text = std::string(prefix) + en.name;
        MakeLabel(r, text.c_str(), en.dir ? 0xFFE066 : kind == ' ' ? 0x8FA3C8 : 0xFFFFFF,
                  LV_ALIGN_LEFT_MID, 12, 0, 386, true);
    }
}

void Radio::OnSdRow(int idx) {
    if (idx < 0 || idx >= (int)sd_entries_.size()) return;
    const SdEntry en = sd_entries_[idx];

    if (en.dir) {
        if (en.name == "..") {
            const size_t slash = sd_path_.rfind('/');
            if (slash != std::string::npos && slash > strlen(kSdBase) - 1) {
                sd_path_ = sd_path_.substr(0, slash);
            }
            if (sd_path_.size() < strlen(kSdBase)) sd_path_ = kSdBase;
        } else {
            sd_path_ += "/" + en.name;
        }
        sd_msg_ = "";
        ReadDir();
        // No se redibuja aqui: esta fila se borraria mientras se procesa su propio evento
        lv_async_call(OnAsyncSd, nullptr);
        return;
    }

    // Archivo: por ahora solo se muestra la informacion
    struct stat st;
    const std::string full = sd_path_ + "/" + en.name;
    char buf[160];
    if (stat(full.c_str(), &st) == 0) {
        const char kind = FileKind(en.name);
        snprintf(buf, sizeof(buf), "%s - %u KB%s", en.name.c_str(), (unsigned)(st.st_size / 1024),
                 kind == 'A' ? " (audio: se podra reproducir en la etapa 3)"
                 : kind == 'V' ? " (video: etapa 4)" : "");
    } else {
        snprintf(buf, sizeof(buf), "%s", en.name.c_str());
    }
    sd_msg_ = buf;
    SetLabel(sd_info_, sd_msg_.c_str());
}

// ---------------------------------------------------------------------------
// Interfaz
// ---------------------------------------------------------------------------

static void MakeDigitSegs(lv_obj_t* parent, lv_obj_t** seg, int x, int y, int w, int h, int t) {
    const int uh = (h - 3 * t) / 2;
    const int hw = w - 2 * t;
    const lv_color_t off = C(kOff);
    seg[0] = Box(parent, x + t, y, hw, t, off, 2);
    seg[1] = Box(parent, x + w - t, y + t, t, uh, off, 2);
    seg[2] = Box(parent, x + w - t, y + 2 * t + uh, t, uh, off, 2);
    seg[3] = Box(parent, x + t, y + h - t, hw, t, off, 2);
    seg[4] = Box(parent, x, y + 2 * t + uh, t, uh, off, 2);
    seg[5] = Box(parent, x, y + t, t, uh, off, 2);
    seg[6] = Box(parent, x + t, y + t + uh, hw, t, off, 2);
}

static void SetDigitSegs(lv_obj_t** seg, uint32_t on, int v) {
    uint8_t mask = 0;
    if (v >= 0 && v <= 9) {
        mask = kSegMask[v];
    } else if (v == -1) {
        mask = 0x40;
    }
    for (int i = 0; i < 7; i++) {
        lv_obj_set_style_bg_color(seg[i], C(((mask >> i) & 1) ? on : kOff), 0);
    }
}

void Radio::BuildUI() {
    root_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, kW, kH);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, C(kBg), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    // --- Fila superior: wifi, hora y bateria ---
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

    const int cx0 = 195;
    MakeDigitSegs(root_, clk_[0].seg, cx0, 14, 16, 30, 4);
    MakeDigitSegs(root_, clk_[1].seg, cx0 + 20, 14, 16, 30, 4);
    Circle(root_, cx0 + 43, 22, 4, C(0xFFFFFF));
    Circle(root_, cx0 + 43, 33, 4, C(0xFFFFFF));
    MakeDigitSegs(root_, clk_[2].seg, cx0 + 54, 14, 16, 30, 4);
    MakeDigitSegs(root_, clk_[3].seg, cx0 + 74, 14, 16, 30, 4);
    for (int i = 0; i < 4; i++) clk_[i].on = 0xFFFFFF;

    // --- Columna izquierda ---
    MakeButton(root_, 20, 66, 88, 44, 0x2EA8F0, 12, "SD", 1, 0xFFFFFF);
    mute_btn_ = MakeButton(root_, 114, 66, 96, 44, 0x2EA8F0, 12, "MUTE", 2, 0xFFFFFF);

    lv_obj_t* np = Box(root_, 20, 120, 190, 108, C(kPanel), 12);
    MakeLabel(np, "AHORA SUENA", 0x6F7FA5, LV_ALIGN_TOP_LEFT, 10, 6, 170, true);
    np_name_ = MakeLabel(np, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 10, 24, 170, true);
    np_status_ = MakeLabel(np, "", 0xFFE066, LV_ALIGN_TOP_LEFT, 10, 60, 170, true);
    for (int i = 0; i < 16; i++) {
        bars_[i] = Box(np, 10 + i * 11, 96, 8, 4, C(0x2EA8F0), 2);
    }

    MakeButton(root_, 20, 238, 190, 54, 0x8B5CF6, 14, "PLAY / STOP", 3, 0xFFFFFF);

    vol_lbl_ = MakeLabel(root_, "", 0xFFFFFF, LV_ALIGN_TOP_LEFT, 20, 304, 190, true);
    Box(root_, 20, 326, 190, 12, C(0x2A3350), 6);
    vol_fill_ = Box(root_, 20, 326, 100, 12, C(0xFFD93D), 6);
    MakeButton(root_, 20, 348, 88, 40, kPanel, 12, "VOL -", 4, 0xFFFFFF);
    MakeButton(root_, 122, 348, 88, 40, kPanel, 12, "VOL +", 5, 0xFFFFFF);

    // --- Columna derecha: emisoras ---
    list_ = Box(root_, 224, 66, 216, 322, C(kListBg), 12);
    MakeScrollable(list_);
    const int n = std::min((int)stations_.size(), kMaxStations);
    for (int i = 0; i < n; i++) {
        row_[i] = MakeRow(list_, 4, 4 + i * 58, 208, 54, kListBg, 100 + i);
        MakeLabel(row_[i], stations_[i].name.c_str(), 0xFFFFFF, LV_ALIGN_TOP_LEFT, 12, 8, 188, true);
        MakeLabel(row_[i], stations_[i].genre.c_str(), 0x8FA3C8, LV_ALIGN_TOP_LEFT, 12, 30, 188, true);
    }

    // --- Panel del explorador de la SD ---
    sd_panel_ = Box(root_, 0, 0, kW, kH, C(kBg), 0);
    lv_obj_add_flag(sd_panel_, LV_OBJ_FLAG_CLICKABLE);
    Show(sd_panel_, false);
    sd_title_ = MakeLabel(sd_panel_, "Tarjeta SD", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 24, 380);
    sd_info_ = MakeLabel(sd_panel_, "", 0xFFE066, LV_ALIGN_TOP_MID, 0, 48, 400);
    sd_list_ = Box(sd_panel_, 30, 80, 420, 316, C(kListBg), 12);
    MakeScrollable(sd_list_);
    MakeButton(sd_panel_, 140, 410, 200, 48, kPanel, 14, "VOLVER", 6, 0xFFFFFF);
}

void Radio::SelectStation(int idx) {
    if (idx < 0 || idx >= (int)stations_.size() || idx >= kMaxStations) return;
    selected_ = idx;
    for (int i = 0; i < (int)stations_.size() && i < kMaxStations; i++) {
        if (row_[i]) lv_obj_set_style_bg_color(row_[i], C(i == selected_ ? kSelected : kListBg), 0);
    }
    SaveSettings();
    RefreshNow();
}

void Radio::RefreshAll() {
    RefreshClock();
    RefreshVolume();
    for (int i = 0; i < (int)stations_.size() && i < kMaxStations; i++) {
        if (row_[i]) lv_obj_set_style_bg_color(row_[i], C(i == selected_ ? kSelected : kListBg), 0);
    }
    RefreshNow();
    UpdateStatusIcons();
}

void Radio::RefreshVolume() {
    char buf[32];
    snprintf(buf, sizeof(buf), muted_ ? "Volumen %d%% (silencio)" : "Volumen %d%%", volume_);
    SetLabel(vol_lbl_, buf);
    lv_obj_set_width(vol_fill_, std::max(2, (190 * volume_) / 100));
    lv_obj_set_style_bg_color(mute_btn_, C(muted_ ? 0xE53935 : 0x2EA8F0), 0);
}

void Radio::RefreshNow() {
    if (selected_ >= 0 && selected_ < (int)stations_.size()) {
        SetLabel(np_name_, stations_[selected_].name.c_str());
    }
    SetLabel(np_status_, playing_ ? "Sin audio aun (etapa 1)" : "Detenido");
}

void Radio::RefreshClock() {
    const time_t now = time(nullptr);
    int h = -1, m = -1;
    if (now >= 1700000000) {
        struct tm t;
        localtime_r(&now, &t);
        h = t.tm_hour;
        m = t.tm_min;
    }
    if (h == hour_ && m == min_) return;
    hour_ = h;
    min_ = m;
    if (h >= 0) {
        SetDigitSegs(clk_[0].seg, clk_[0].on, h / 10);
        SetDigitSegs(clk_[1].seg, clk_[1].on, h % 10);
        SetDigitSegs(clk_[2].seg, clk_[2].on, m / 10);
        SetDigitSegs(clk_[3].seg, clk_[3].on, m % 10);
    } else {
        for (int i = 0; i < 4; i++) SetDigitSegs(clk_[i].seg, clk_[i].on, -1);
    }
}

void Radio::UpdateStatusIcons() {
    int bars = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        const int r = ap.rssi;
        bars = r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
    }
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(wifi_bar_[i], C(i < bars ? 0xFFFFFF : kOff), 0);
    }

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
// Botones
// ---------------------------------------------------------------------------

void Radio::HandleButton(int id) {
    Activity();

    if (id >= 1000) {  // fila del explorador de la SD
        OnSdRow(id - 1000);
        return;
    }
    if (id >= 100 && id < 100 + kMaxStations) {  // emisora
        SelectStation(id - 100);
        return;
    }

    switch (id) {
        case 1:  // SD
            OpenSd();
            break;
        case 2:  // silencio
            muted_ = !muted_;
            RefreshVolume();
            break;
        case 3:  // play / stop
            playing_ = !playing_;
            RefreshNow();
            break;
        case 4:
            volume_ = std::max(0, volume_ - 10);
            SaveSettings();
            RefreshVolume();
            break;
        case 5:
            volume_ = std::min(100, volume_ + 10);
            SaveSettings();
            RefreshVolume();
            break;
        case 6:  // volver desde la SD
            Show(sd_panel_, false);
            break;
        default:
            break;
    }
}
