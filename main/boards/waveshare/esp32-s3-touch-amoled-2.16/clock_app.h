#pragma once

#include <lvgl.h>

#include <functional>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

class Display;

// Modo reposo con reloj (5 estilos que cambian al deslizar el dedo) y despertador
class ClockApp {
public:
    static ClockApp& GetInstance();

    // Llamar una vez, despues de crear la pantalla, el tactil y la radio.
    void Init(Display* display);
    // auto_standby = true cuando entra solo por inactividad: un toque vuelve al modo anterior.
    void SetVisible(bool visible, bool auto_standby = false);
    bool IsVisible() const { return visible_; }
    // true si la placa no debe apagarse sola (reloj en pantalla, sonando o con alarmas activas)
    bool ShouldStayOn() const { return visible_ || ringing_ || any_enabled_; }
    void SetActivityCallback(std::function<void()> cb) { on_activity_ = std::move(cb); }
    // Se llama cuando un toque debe sacar del reposo automatico
    void SetExitCallback(std::function<void()> cb) { on_exit_ = std::move(cb); }
    // Se llama cada segundo (la placa lo usa para decidir cuando pasar al reposo)
    void SetTickCallback(std::function<void()> cb) { on_tick_ = std::move(cb); }

    // Internos: los llaman los eventos de LVGL.
    void Tick();
    void HandleButton(int id);
    void OnTouch(bool pressed, int x, int y);

private:
    ClockApp() = default;

    struct Alarm {
        uint8_t enabled;
        uint8_t hour;
        uint8_t minute;
        uint8_t days;     // bit0 = lunes ... bit6 = domingo; 0 = una sola vez
        uint8_t src;      // 0 pitido, 1 radio, 2 MP3 de la SD
        uint8_t station;  // emisora (si src = 1)
        uint8_t pad0, pad1;
        char file[48];    // archivo MP3 (si src = 2)
    };
    struct Digit {
        lv_obj_t* seg[7];
        uint32_t on;
        uint32_t off;
    };

    static constexpr int kStyles = 5;
    static constexpr int kMaxAlarms = 4;

    void Activity();
    void LoadSettings();
    void SaveSettings();
    void ReadTime();
    bool TimeTrusted() const;

    // Interfaz
    void BuildUI();
    void MakeDigit(Digit& d, lv_obj_t* parent, int x, int y, int w, int h, int t, uint32_t on,
                   uint32_t off);
    void SetDigit(Digit& d, int v);
    void ShowStyle();
    void UpdateFace(bool force);
    void UpdateAlarmLabel();
    void RefreshAlarmList();
    void OpenEditor(int idx);
    void RefreshEditor();
    void ScanMp3();
    std::string SourceText(const Alarm& a) const;
    std::string DaysText(const Alarm& a) const;

    // Alarma
    void CheckAlarms();
    void StartRinging(int idx, bool from_snooze);
    void StopRinging();
    void Snooze();
    void RingTick();
    void StartSound(const Alarm& a);
    void StopSound();
    void Beep();

    Display* display_ = nullptr;
    std::function<void()> on_activity_;
    std::function<void()> on_exit_;
    std::function<void()> on_tick_;

    bool visible_ = false;
    bool auto_standby_ = false;
    bool time_ok_ = false;
    int h_ = 0, m_ = 0, s_ = 0, wday_ = 0, mday_ = 1, mon_ = 1, year_ = 2026;
    int last_min_ = -1;
    int style_ = 0;
    int touch_x0_ = 0, touch_y0_ = 0;

    Alarm alarms_[kMaxAlarms] = {};
    bool any_enabled_ = false;
    int last_fire_key_[kMaxAlarms] = {-1, -1, -1, -1};

    bool ringing_ = false;
    int ring_idx_ = -1;
    int ring_secs_ = 0;
    int sound_retries_ = 0;
    bool use_beep_ = false;
    int snooze_idx_ = -1;
    int64_t snooze_until_ = 0;

    int edit_idx_ = -1;
    Alarm edit_ = {};
    bool test_playing_ = false;
    std::vector<std::string> mp3_files_;
    std::string mp3_dir_;
    std::string sd_note_;

    // Widgets
    lv_obj_t* root_ = nullptr;
    lv_obj_t* face_[kStyles] = {};
    lv_obj_t* style_dot_[kStyles] = {};
    lv_obj_t* alarm_lbl_ = nullptr;
    lv_obj_t* bell_btn_ = nullptr;

    // Estilo 0: digital grande
    Digit d0_[4] = {};
    lv_obj_t* colon0_[2] = {};
    lv_obj_t* sec_fill_ = nullptr;
    lv_obj_t* date0_ = nullptr;
    // Estilo 1: analogico
    lv_obj_t* hand_h_[9] = {};
    lv_obj_t* hand_m_[12] = {};
    lv_obj_t* hand_s_[20] = {};
    lv_obj_t* date1_ = nullptr;
    // Estilo 2: anillo de segundos
    lv_obj_t* ring_dot_[60] = {};
    Digit d2_[4] = {};
    lv_obj_t* colon2_[2] = {};
    lv_obj_t* date2_ = nullptr;
    // Estilo 3: tarjetas
    Digit d3_[4] = {};
    lv_obj_t* date3_ = nullptr;
    // Estilo 4: noche
    Digit d4_[4] = {};
    lv_obj_t* colon4_[2] = {};
    lv_obj_t* date4_ = nullptr;

    // Panel de alarmas y editor
    lv_obj_t* alarm_panel_ = nullptr;
    lv_obj_t* row_lbl_[kMaxAlarms] = {};
    lv_obj_t* row_tog_[kMaxAlarms] = {};
    lv_obj_t* edit_panel_ = nullptr;
    lv_obj_t* ed_title_ = nullptr;
    lv_obj_t* ed_hour_ = nullptr;
    lv_obj_t* ed_min_ = nullptr;
    lv_obj_t* ed_day_[7] = {};
    lv_obj_t* ed_src_ = nullptr;
    lv_obj_t* ed_sel_ = nullptr;
    lv_obj_t* ed_test_ = nullptr;

    // Aviso a pantalla completa
    lv_obj_t* ring_panel_ = nullptr;
    lv_obj_t* ring_txt_ = nullptr;
    Digit ring_d_[4] = {};
};
