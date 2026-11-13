#pragma once

#include <lvgl.h>

#include <functional>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

class Display;

class Radio {
public:
    static Radio& GetInstance();

    // Llamar una vez, despues de crear la pantalla y el tactil.
    void Init(Display* display);
    void SetVisible(bool visible);
    bool IsVisible() const { return visible_; }
    void SetActivityCallback(std::function<void()> cb) { on_activity_ = std::move(cb); }
    // Devuelve el nivel de bateria (0-100) y si esta cargando.
    void SetBatteryProvider(std::function<bool(int&, bool&)> cb) { battery_cb_ = std::move(cb); }

    // Internos: los llaman los eventos de LVGL.
    void Tick();
    void HandleButton(int id);
    void RebuildSdList();

private:
    Radio() = default;

    struct Station {
        std::string name;
        std::string genre;
        std::string url;
    };
    struct SdEntry {
        std::string name;
        bool dir;
    };
    struct Digit {
        lv_obj_t* seg[7];
        uint32_t on;
    };

    void Activity();
    void LoadSettings();
    void SaveSettings();
    void LoadStations();

    bool MountSd();
    void ReadDir();
    void OpenSd();
    void OnSdRow(int idx);

    void BuildUI();
    void SelectStation(int idx);
    void RefreshAll();
    void RefreshVolume();
    void RefreshNow();
    void RefreshClock();
    void UpdateStatusIcons();

    static constexpr int kMaxStations = 40;
    static constexpr int kMaxSdEntries = 200;

    Display* display_ = nullptr;
    std::function<void()> on_activity_;
    std::function<bool(int&, bool&)> battery_cb_;

    bool visible_ = false;
    bool playing_ = false;
    bool muted_ = false;
    int volume_ = 70;
    int selected_ = 0;
    int hour_ = -1, min_ = -1;
    uint32_t tick_count_ = 0;

    std::vector<Station> stations_;

    bool sd_mounted_ = false;
    void* sd_card_ = nullptr;
    std::string sd_path_;
    std::vector<SdEntry> sd_entries_;
    std::string sd_msg_;

    // Widgets
    lv_obj_t* root_ = nullptr;
    lv_obj_t* wifi_bar_[4] = {};
    lv_obj_t* bat_fill_ = nullptr;
    lv_obj_t* bat_lbl_ = nullptr;
    Digit clk_[4] = {};
    lv_obj_t* mute_btn_ = nullptr;
    lv_obj_t* np_name_ = nullptr;
    lv_obj_t* np_status_ = nullptr;
    lv_obj_t* bars_[16] = {};
    lv_obj_t* vol_lbl_ = nullptr;
    lv_obj_t* vol_fill_ = nullptr;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* row_[kMaxStations] = {};

    lv_obj_t* sd_panel_ = nullptr;
    lv_obj_t* sd_title_ = nullptr;
    lv_obj_t* sd_info_ = nullptr;
    lv_obj_t* sd_list_ = nullptr;
};
