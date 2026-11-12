#pragma once

#include <lvgl.h>

#include <functional>
#include <stdint.h>
#include <utility>

class Display;

// 1 = tiempo real. Para probar rapido pon 60 (1 minuto real = 1 hora de juego).
#ifndef TAMA_TIME_SCALE
#define TAMA_TIME_SCALE 1
#endif

class Tamagotchi {
public:
    static Tamagotchi& GetInstance();

    // Llamar una vez, despues de crear la pantalla y el tactil.
    void Init(Display* display);
    // Muestra u oculta la pantalla de la mascota (boton GPIO 18).
    void Toggle();
    bool IsVisible() const { return visible_; }
    // Se llama cada vez que el usuario interactua (para despertar la pantalla).
    void SetActivityCallback(std::function<void()> cb) { on_activity_ = std::move(cb); }

    // Internos: los llaman los eventos de LVGL.
    void Tick();
    void HandleButton(int id);
    void OnRootTap();

private:
    Tamagotchi() = default;

    struct State {
        uint32_t magic;
        uint8_t alive, stage, species;           // stage: 0 huevo, 1 bebe, 2 nino, 3 adulto, 4 anciano
        uint8_t hunger, happy, discipline, weight;
        uint8_t poop, sick, meds, mistakes;
        uint8_t asleep, lights_off, adult_good, call;  // call: 0 nada, 1 necesidad, 2 berrinche
        uint8_t sleep_light_flag, death_cause, pad0, pad1;
        uint32_t age_sec, stage_sec;
        uint32_t hunger_acc, happy_acc, poop_acc;
        uint32_t neglect_sec, sick_sec, sick_check_acc;
        uint32_t tantrum_acc, tantrum_target;
        uint32_t call_sec, sleep_light_sec;
        uint32_t saved_unix, died_age_sec;
    };

    // Logica
    void Load();
    void Save();
    void NewEgg();
    void Hatch();
    void Evolve(int stage);
    void Die(int cause);
    void Step(uint32_t dt);
    void TryCatchUp();
    void RaiseCall(uint8_t type);
    void UpdateCall();
    void AddMistake();
    void Activity();
    void SetMessage(const char* text, int frames);
    void Feed(bool snack);
    void ShowPopup(bool show);
    void ShowGamePopup(bool show);
    const char* StatusText() const;

    // Minijuego
    void StartGame();
    void NextRound();
    void OnGamePick(int side);
    void GameTick();
    void EndGame(bool finished);
    void BeginMini(int type);
    void StartCatch();
    void StartBag();
    void StartMemo();
    void MiniTick();
    void OnMiniTap(int k);
    void FinishMini(bool win);
    void SetPads(int lit);

    // Dibujo
    void BuildUI();
    void BuildSprite();
    void BuildEgg();
    void BuildTomb();
    void UpdateFace();
    void RefreshUI();
    void UpdateBackground();
    uint32_t SpriteKey() const;
    bool EggCracked() const;

    Display* display_ = nullptr;
    State st_{};
    std::function<void()> on_activity_;

    bool visible_ = false;
    bool notify_ = false;
    bool catchup_pending_ = false;
    bool blink_ = false;
    bool game_active_ = false;
    bool game_awaiting_ = false;
    int game_round_ = 0;
    int game_wins_ = 0;
    int game_target_ = 0;
    int game_wait_frames_ = 0;
    int look_ = 0;
    int eat_frames_ = 0;
    int msg_frames_ = 0;
    int popup_frames_ = 0;
    uint32_t frame_ = 0;
    uint32_t since_save_ = 0;
    uint32_t sprite_key_ = 0xFFFFFFFF;
    int64_t last_s_ = 0;
    char msg_[64] = {0};
    char game_text_[80] = {0};
    int game_type_ = 0;  // 0 ninguno, 1 mirar, 2 atrapa, 3 saco, 4 memoria
    int game_score_ = 0;
    int game_frames_ = 0;
    int catch_life_ = 0;
    int bag_pulse_ = 0;
    int memo_len_ = 2;
    int memo_phase_ = 0;
    int memo_timer_ = 0;
    int memo_input_ = 0;
    int memo_flash_ = 0;
    bool memo_ok_ = false;
    uint8_t memo_seq_[8] = {0};

    // Widgets
    lv_obj_t* root_ = nullptr;
    lv_obj_t* cont_ = nullptr;
    lv_obj_t* name_lbl_ = nullptr;
    lv_obj_t* stats_lbl_ = nullptr;
    lv_obj_t* msg_lbl_ = nullptr;
    lv_obj_t* zzz_ = nullptr;
    lv_obj_t* dark_ = nullptr;
    lv_obj_t* popup_ = nullptr;
    lv_obj_t* game_ = nullptr;
    lv_obj_t* alert_ = nullptr;
    lv_obj_t* gpopup_ = nullptr;
    lv_obj_t* game_area_ = nullptr;
    lv_obj_t* target_ = nullptr;
    lv_obj_t* pads_[4] = {};
    lv_obj_t* sky_ = nullptr;
    lv_obj_t* ground_ = nullptr;
    lv_obj_t* body_ = nullptr;  // sol o luna
    lv_obj_t* stars_[10] = {};
    int bg_key_ = -1;
    lv_obj_t* hunger_dot_[4] = {};
    lv_obj_t* happy_dot_[4] = {};
    lv_obj_t* poop_[4][2] = {};

    // Cara del sprite actual
    lv_obj_t* eye_[2] = {};
    lv_obj_t* pup_[2] = {};
    lv_obj_t* mouth_ = nullptr;
    int ed_ = 0, pd_ = 0, eye_y_ = 0, face_cx_ = 0, mouth_y_ = 0, bw_ = 0, bh_ = 0;
    int eye_x_[2] = {0, 0};
};
