#pragma once

#include <lvgl.h>

#include <functional>
#include <stdint.h>
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
    // Se llama cada vez que el usuario toca algo (para despertar la pantalla).
    void SetActivityCallback(std::function<void()> cb) { on_activity_ = std::move(cb); }
    // Para la siguiente etapa (temperatura por internet).
    void SetTemperature(int celsius, bool valid);

    // Internos: los llaman los eventos de LVGL.
    void Tick();
    void HandleButton(int id);

private:
    Agenda() = default;

    struct Event {
        uint16_t year;
        uint8_t month, day, hour, minute, remind, pad;
        char text[40];
    };
    struct Digit {
        lv_obj_t* seg[7];
        uint32_t on;
    };

    void Activity();
    void LoadEvents();
    void SaveEvents();
    void ReadClock();
    bool HasEvents(int y, int m, int d) const;
    int NowKey() const;

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
    void SaveEvent();
    void DeleteEvent(int idx);
    void OpenUpcoming();
    void CloseAllPanels();

    static constexpr int kMaxRows = 6;
    static constexpr int kUpRows = 7;

    Display* display_ = nullptr;
    std::function<void()> on_activity_;

    bool visible_ = false;
    bool time_ok_ = false;
    int today_y_ = 0, today_m_ = 0, today_d_ = 0, hour_ = -1, min_ = -1;
    int view_y_ = 0, view_m_ = 0, sel_day_ = 0;
    int temp_c_ = 0;
    bool temp_ok_ = false;

    std::vector<Event> events_;
    int day_idx_[kMaxRows] = {};
    int day_count_ = 0;
    int edit_h_ = 9, edit_m_ = 0;
    bool edit_remind_ = true;

    // Widgets
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
    lv_obj_t* ta_ = nullptr;
    lv_obj_t* kb_ = nullptr;

    lv_obj_t* up_panel_ = nullptr;
    lv_obj_t* up_title_ = nullptr;
    lv_obj_t* up_lbl_[kUpRows] = {};
};
