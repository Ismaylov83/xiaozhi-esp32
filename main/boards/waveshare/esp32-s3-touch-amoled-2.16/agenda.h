#pragma once

#include <lvgl.h>

#include <atomic>
#include <functional>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

class Display;

class Agenda {
public:
    static Agenda& GetInstance();

    // Llamar una vez, despues de crear la pantalla y el tactil.
    void Init(Display* display);
    void SetVisible(bool visible);
    bool IsVisible() const { return visible_; }
    // true si hay algun recordatorio por sonar (la placa no debe apagarse sola)
    bool HasPendingReminders() const { return pending_reminders_.load(); }
    // Se llama cada vez que el usuario toca algo o suena un aviso (despierta la pantalla).
    void SetActivityCallback(std::function<void()> cb) { on_activity_ = std::move(cb); }
    // Devuelve el nivel de bateria (0-100) y si esta cargando.
    void SetBatteryProvider(std::function<bool(int&, bool&)> cb) { battery_cb_ = std::move(cb); }
    // Para la siguiente etapa (temperatura por internet).
    void SetTemperature(int celsius, bool valid);

    // Internos: los llaman los eventos de LVGL.
    void Tick();
    void HandleButton(int id);

private:
    Agenda() = default;

    struct Event {
        uint16_t year;
        uint8_t month, day, hour, minute, remind, fired;
        char text[40];
    };
    struct Digit {
        lv_obj_t* seg[7];
        uint32_t on;
    };

    void Activity();
    void LoadEvents();
    void SaveEvents();
    void SortEvents();
    void ReadClock();
    bool HasEvents(int y, int m, int d) const;
    int NowKey() const;

    // Avisos
    void CheckAlarms();
    void StartAlarm(const Event& e);
    void DismissAlarm();
    void AlarmTick();
    void Beep();
    void UpdateAvisos();
    void UpdateStatusIcons();

    // Herramientas para que xiaozhi use la agenda por voz
    void RegisterTools();
    std::string AddEventApi(int y, int m, int d, int h, int mi, const std::string& text,
                            bool remind);
    std::string ListEventsApi(int y, int m, int d);
    std::string DeleteEventApi(int y, int m, int d, int h, int mi);
    void RefreshAfterChange();

    // Interfaz
    void BuildUI();
    void MakeDigit(Digit& d, lv_obj_t* parent, int x, int y, int w, int h, int t, uint32_t on);
    void SetDigit(Digit& d, int v);
    void RefreshAll();
    void RefreshCalendar();
    void RefreshClock();
    void UpdateNext();
    void OpenDay(int day);
    void RefreshDayPanel();
    void OpenEditor();
    void UpdateEditLabels();
    void UpdateEditText();
    void TypeKey(int id);
    void SaveEvent();
    void DeleteEvent(int idx);
    void OpenUpcoming();
    void CloseAllPanels();

    static constexpr int kMaxRows = 6;
    static constexpr int kUpRows = 7;

    Display* display_ = nullptr;
    std::function<void()> on_activity_;
    std::function<bool(int&, bool&)> battery_cb_;

    bool visible_ = false;
    bool time_ok_ = false;
    int today_y_ = 0, today_m_ = 0, today_d_ = 0, hour_ = -1, min_ = -1;
    int view_y_ = 0, view_m_ = 0, sel_day_ = 0;
    int temp_c_ = 0;
    bool temp_ok_ = false;
    uint32_t tick_count_ = 0;

    std::vector<Event> events_;
    std::atomic<bool> pending_reminders_{false};
    int day_idx_[kMaxRows] = {};
    int day_count_ = 0;
    int edit_h_ = 9, edit_m_ = 0;
    bool edit_remind_ = true;
    int edit_len_ = 0;
    char edit_text_[40] = {0};

    bool alarm_active_ = false;
    int alarm_secs_ = 0;

    // Widgets: pantalla principal
    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* cell_[42] = {};
    lv_obj_t* cell_lbl_[42] = {};
    lv_obj_t* cell_dot_[42] = {};
    Digit clk_[4] = {};
    Digit temp_[3] = {};
    lv_obj_t* colon_[2] = {};
    lv_obj_t* date_lbl_ = nullptr;
    lv_obj_t* next_lbl_ = nullptr;
    lv_obj_t* avisos_btn_ = nullptr;
    lv_obj_t* wifi_bar_[4] = {};
    lv_obj_t* bat_fill_ = nullptr;
    lv_obj_t* bat_lbl_ = nullptr;

    // Paneles
    lv_obj_t* day_panel_ = nullptr;
    lv_obj_t* day_title_ = nullptr;
    lv_obj_t* day_empty_ = nullptr;
    lv_obj_t* row_[kMaxRows] = {};
    lv_obj_t* row_lbl_[kMaxRows] = {};

    lv_obj_t* edit_panel_ = nullptr;
    lv_obj_t* edit_title_ = nullptr;
    lv_obj_t* edit_h_lbl_ = nullptr;
    lv_obj_t* edit_m_lbl_ = nullptr;
    lv_obj_t* edit_remind_btn_ = nullptr;
    lv_obj_t* edit_txt_lbl_ = nullptr;

    lv_obj_t* up_panel_ = nullptr;
    lv_obj_t* up_title_ = nullptr;
    lv_obj_t* up_lbl_[kUpRows] = {};

    lv_obj_t* fc_panel_ = nullptr;

    lv_obj_t* alarm_panel_ = nullptr;
    lv_obj_t* alarm_txt_ = nullptr;
    Digit alarm_clk_[4] = {};
};
