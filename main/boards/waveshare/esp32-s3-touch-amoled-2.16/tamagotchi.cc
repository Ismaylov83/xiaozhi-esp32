#include "tamagotchi.h"
#include "display/display.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG "Tamagotchi"

namespace {

constexpr int kW = 480;
constexpr int kH = 480;
constexpr int kNumSpecies = 8;
constexpr int kBtnCount = 6;

// Zona donde se dibuja la mascota
constexpr int kContX = 120;
constexpr int kContY = 96;
constexpr int kContW = 240;
constexpr int kContH = 250;
constexpr int kBottom = 245;

constexpr uint32_t kMagic = 0x54414D33;

// Tiempos (segundos de juego)
constexpr uint32_t kEggSec = 60;
constexpr uint32_t kBabySec = 2 * 3600;
constexpr uint32_t kChildSec = 24 * 3600;
constexpr uint32_t kAdultSec = 72 * 3600;
constexpr uint32_t kSeniorSec = 48 * 3600;
constexpr uint32_t kDaySec = 24 * 3600;
constexpr uint32_t kAwakeSec = 16 * 3600;   // 16 h despierto, 8 h dormido
constexpr uint32_t kHungerSec = 2400;       // 1 corazon de hambre cada 40 min
constexpr uint32_t kHappySec = 3000;        // 1 corazon de felicidad cada 50 min
constexpr uint32_t kPoopSec = 5400;         // una caca cada 90 min
constexpr uint32_t kNeglectDeathSec = 8 * 3600;
constexpr uint32_t kSickCheckSec = 600;
constexpr uint32_t kSickDeathSec = 4 * 3600;
constexpr uint32_t kCallTimeoutSec = 900;
constexpr uint32_t kSleepLightSec = 1800;
constexpr uint32_t kMaxCatchUpSec = 10 * 3600;
constexpr uint32_t kSaveEverySec = 300;

// Cambia esta fuente si el texto se ve pequeno (por ejemplo &lv_font_montserrat_20 si esta activada)
const lv_font_t* kFont = LV_FONT_DEFAULT;

const char* kStageNames[5] = {"Huevo", "Bebe", "Nino", "Adulto", "Anciano"};

struct SpeciesInfo {
    const char* name;
    uint32_t body, belly, accent;
};

// El orden es el mismo que el de las formas: gato, rana, pollito, oso, alien, conejo, dino, fantasma
const SpeciesInfo kSpecies[kNumSpecies] = {
    {"Pelusa", 0xFF8FB1, 0xFFE0EA, 0xFF5C8D},
    {"Sapito", 0x5BD66B, 0xD8F7B8, 0x2E9E4F},
    {"Pollito", 0xFFD93D, 0xFFF3B0, 0xFF8A00},
    {"Osito", 0xB5835A, 0xE8CDAE, 0x6B4423},
    {"Alien", 0xA66CFF, 0xDCC8FF, 0x5CFFB0},
    {"Conejo", 0xF4F4F8, 0xFFFFFF, 0xFF9EB5},
    {"Dino", 0x37C9C0, 0xCFF5F1, 0xFF7A3D},
    {"Fantasma", 0xBFC8FF, 0xFFFFFF, 0x7A86E8},
};

struct BtnDef {
    const char* text;
    uint32_t color;
};

const BtnDef kBtns[kBtnCount] = {
    {"COME", 0xFF9F43}, {"JUEGO", 0x54A0FF}, {"LUZ", 0xFECA57},
    {"MEDI", 0xFF6B6B}, {"LIMP", 0x1DD1A1},  {"DISC", 0xA29BFE},
};

uint32_t Rnd(uint32_t n) { return esp_random() % n; }

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

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, uint32_t color, lv_align_t align,
                    int x, int y, int width = 0) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, C(color), 0);
    lv_obj_set_style_text_font(l, kFont, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (width > 0) lv_obj_set_width(l, width);
    lv_obj_align(l, align, x, y);
    return l;
}

lv_obj_t* MakeDot(lv_obj_t* parent, int x, int y, uint32_t color) {
    lv_obj_t* o = Circle(parent, x, y, 20, C(color));
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, C(color), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    return o;
}

void SetDot(lv_obj_t* d, bool filled) {
    lv_obj_set_style_bg_opa(d, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void OnBtnEvent(lv_event_t* e) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    Tamagotchi::GetInstance().HandleButton(id);
}

void OnRootClick(lv_event_t*) { Tamagotchi::GetInstance().OnRootTap(); }

void OnTimer(lv_timer_t*) { Tamagotchi::GetInstance().Tick(); }

lv_obj_t* MakeButton(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius,
                     const char* text, int id) {
    lv_obj_t* b = Box(parent, x, y, w, h, C(color), radius);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, OnBtnEvent, LV_EVENT_CLICKED, (void*)(intptr_t)id);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, C(0x101018), 0);
    lv_obj_set_style_text_font(l, kFont, 0);
    lv_obj_center(l);
    return b;
}

// Objeto que reacciona en cuanto se toca (para los minijuegos)
lv_obj_t* MakeTouch(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius,
                    int id) {
    lv_obj_t* o = Box(parent, x, y, w, h, C(color), radius);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, OnBtnEvent, LV_EVENT_PRESSED, (void*)(intptr_t)id);
    return o;
}

}  // namespace

Tamagotchi& Tamagotchi::GetInstance() {
    static Tamagotchi instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Inicio, guardado y carga
// ---------------------------------------------------------------------------

void Tamagotchi::Init(Display* display) {
    if (root_ || !display) return;
    display_ = display;
    DisplayLockGuard lock(display_);
    Load();
    BuildUI();
    last_s_ = esp_timer_get_time() / 1000000;
    lv_timer_create(OnTimer, 250, nullptr);
    ESP_LOGI(TAG, "Tamagotchi listo (vivo=%d etapa=%d)", st_.alive, st_.stage);
}

void Tamagotchi::Load() {
    bool ok = false;
    nvs_handle_t h;
    if (nvs_open("tamagotchi", NVS_READONLY, &h) == ESP_OK) {
        State tmp;
        size_t size = sizeof(State);
        if (nvs_get_blob(h, "state", &tmp, &size) == ESP_OK && size == sizeof(State) &&
            tmp.magic == kMagic) {
            st_ = tmp;
            ok = true;
        }
        nvs_close(h);
    }
    if (ok) {
        catchup_pending_ = true;
    } else {
        NewEgg();
    }
}

void Tamagotchi::Save() {
    const time_t now = time(nullptr);
    if (now >= 1700000000) st_.saved_unix = (uint32_t)now;
    nvs_handle_t h;
    if (nvs_open("tamagotchi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "state", &st_, sizeof(st_));
    nvs_commit(h);
    nvs_close(h);
}

void Tamagotchi::TryCatchUp() {
    if (!catchup_pending_) return;
    const time_t now = time(nullptr);
    if (now < 1700000000) return;  // el reloj aun no tiene la hora
    catchup_pending_ = false;
    if (st_.saved_unix > 0 && (uint32_t)now > st_.saved_unix) {
        uint32_t elapsed = (uint32_t)now - st_.saved_unix;
        if (elapsed > kMaxCatchUpSec) elapsed = kMaxCatchUpSec;
        ESP_LOGI(TAG, "Recuperando %u s de tiempo apagado", (unsigned)elapsed);
        while (elapsed > 0) {
            const uint32_t c = elapsed > 60 ? 60 : elapsed;
            Step(c);
            elapsed -= c;
        }
    }
}

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------

void Tamagotchi::NewEgg() {
    memset(&st_, 0, sizeof(st_));
    st_.magic = kMagic;
    st_.alive = 1;
    st_.stage = 0;
    st_.tantrum_target = 7200 + Rnd(7200);
    sprite_key_ = 0xFFFFFFFF;
    Save();
}

void Tamagotchi::Hatch() {
    st_.stage = 1;
    st_.stage_sec = 0;
    st_.age_sec = 0;
    st_.species = (uint8_t)Rnd(kNumSpecies);  // personaje al azar
    st_.hunger = 3;
    st_.happy = 3;
    st_.discipline = 0;
    st_.weight = 5;
    st_.poop = 0;
    st_.sick = 0;
    st_.meds = 0;
    st_.mistakes = 0;
    st_.asleep = 0;
    st_.lights_off = 0;
    st_.call = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "Ha nacido %s!", kSpecies[st_.species].name);
    SetMessage(buf, 20);
    notify_ = true;
    Save();
}

void Tamagotchi::Evolve(int stage) {
    st_.stage = (uint8_t)stage;
    st_.stage_sec = 0;
    if (stage == 3) st_.adult_good = (st_.mistakes <= 4 && st_.discipline >= 40) ? 1 : 0;
    const uint8_t base[5] = {0, 5, 10, 20, 20};
    if (st_.weight < base[stage]) st_.weight = base[stage];
    SetMessage(stage == 4 ? "Se hace viejito..." : "Ha evolucionado!", 20);
    notify_ = true;
    Save();
}

void Tamagotchi::Die(int cause) {
    st_.alive = 0;
    st_.death_cause = (uint8_t)cause;
    st_.died_age_sec = st_.age_sec;
    st_.call = 0;
    st_.asleep = 0;
    st_.lights_off = 0;
    notify_ = true;
    Save();
}

void Tamagotchi::AddMistake() {
    if (st_.mistakes < 99) st_.mistakes++;
}

void Tamagotchi::RaiseCall(uint8_t type) {
    if (st_.call == 0) {
        st_.call = type;
        st_.call_sec = 0;
        notify_ = true;
    }
}

void Tamagotchi::UpdateCall() {
    if (st_.call == 1 && st_.hunger > 0 && st_.happy > 0 && !st_.sick) {
        st_.call = 0;
        st_.call_sec = 0;
    }
}

void Tamagotchi::Step(uint32_t dt) {
    if (!st_.alive) return;
    st_.stage_sec += dt;

    if (st_.stage == 0) {
        if (st_.stage_sec >= kEggSec) Hatch();
        return;
    }
    st_.age_sec += dt;

    // Sueno: 16 h despierto y 8 h dormido, contando desde que nace
    const bool want_sleep = (st_.age_sec % kDaySec) >= kAwakeSec;
    if (want_sleep != (st_.asleep != 0)) {
        st_.asleep = want_sleep ? 1 : 0;
        st_.sleep_light_sec = 0;
        st_.sleep_light_flag = 0;
        if (want_sleep) {
            if (st_.call == 2) st_.call = 0;
        } else {
            st_.lights_off = 0;
        }
    }

    if (st_.asleep) {
        if (!st_.lights_off) {
            st_.sleep_light_sec += dt;
            if (st_.sleep_light_sec >= kSleepLightSec && !st_.sleep_light_flag) {
                st_.sleep_light_flag = 1;
                AddMistake();
                if (st_.happy > 0) st_.happy--;
            }
        } else {
            st_.sleep_light_sec = 0;
        }
    } else {
        st_.hunger_acc += dt;
        while (st_.hunger_acc >= kHungerSec) {
            st_.hunger_acc -= kHungerSec;
            if (st_.hunger > 0) st_.hunger--;
        }
        st_.happy_acc += dt;
        while (st_.happy_acc >= kHappySec) {
            st_.happy_acc -= kHappySec;
            if (st_.happy > 0) st_.happy--;
        }
        st_.poop_acc += dt;
        if (st_.poop_acc >= kPoopSec) {
            st_.poop_acc = 0;
            if (st_.poop < 4) st_.poop++;
        }

        // Descuido: hambre o tristeza a cero durante mucho tiempo = muere
        if (st_.hunger == 0 || st_.happy == 0) {
            st_.neglect_sec += dt;
        } else {
            st_.neglect_sec = 0;
        }
        if (st_.neglect_sec >= kNeglectDeathSec) {
            Die(0);
            return;
        }

        // Enfermedad
        st_.sick_check_acc += dt;
        if (st_.sick_check_acc >= kSickCheckSec) {
            st_.sick_check_acc = 0;
            if (!st_.sick) {
                uint32_t risk = 2;
                if (st_.poop >= 3) risk += 12;
                if (st_.hunger == 0 || st_.happy == 0) risk += 25;
                if (st_.weight > 40) risk += 15;
                if (Rnd(100) < risk) {
                    st_.sick = 1;
                    st_.meds = 0;
                    st_.sick_sec = 0;
                }
            }
        }
        if (st_.sick) {
            st_.sick_sec += dt;
            if (st_.sick_sec >= kSickDeathSec) {
                Die(1);
                return;
            }
        }

        // Llamadas de atencion
        if (st_.call == 0) {
            if (st_.hunger == 0 || st_.happy == 0 || st_.sick) {
                RaiseCall(1);
            } else {
                st_.tantrum_acc += dt;
                if (st_.tantrum_acc >= st_.tantrum_target) {
                    st_.tantrum_acc = 0;
                    st_.tantrum_target = 7200 + Rnd(7200);
                    RaiseCall(2);
                }
            }
        }
        if (st_.call != 0) {
            st_.call_sec += dt;
            if (st_.call_sec >= kCallTimeoutSec) {
                st_.call_sec = 0;
                AddMistake();
                if (st_.call == 2) st_.call = 0;
            }
        }
    }
    UpdateCall();

    // Evolucion y vejez
    if (st_.stage == 1 && st_.stage_sec >= kBabySec) {
        Evolve(2);
    } else if (st_.stage == 2 && st_.stage_sec >= kChildSec) {
        Evolve(3);
    } else if (st_.stage == 3 && st_.stage_sec >= kAdultSec) {
        Evolve(4);
    } else if (st_.stage == 4 && st_.stage_sec >= kSeniorSec) {
        Die(2);
    }
}

// ---------------------------------------------------------------------------
// Acciones del usuario
// ---------------------------------------------------------------------------

void Tamagotchi::Activity() {
    if (on_activity_) on_activity_();
}

void Tamagotchi::SetMessage(const char* text, int frames) {
    snprintf(msg_, sizeof(msg_), "%s", text);
    msg_frames_ = frames;
}

void Tamagotchi::Toggle() {
    if (!display_ || !root_) return;
    DisplayLockGuard lock(display_);
    visible_ = !visible_;
    if (visible_) {
        notify_ = false;
        Show(root_, true);
        Show(alert_, false);
        RefreshUI();
    } else {
        ShowPopup(false);
        ShowGamePopup(false);
        if (game_active_) EndGame(false);
        Show(root_, false);
    }
    Activity();
}

void Tamagotchi::OnRootTap() {
    Activity();
    if (!st_.alive) {
        NewEgg();
        SetMessage("Un huevo nuevo!", 12);
        RefreshUI();
    }
}

void Tamagotchi::ShowPopup(bool show) {
    Show(popup_, show);
    if (show) Show(gpopup_, false);
    popup_frames_ = show ? 20 : 0;
}

void Tamagotchi::ShowGamePopup(bool show) {
    Show(gpopup_, show);
    if (show) Show(popup_, false);
    popup_frames_ = show ? 20 : 0;
}

void Tamagotchi::Feed(bool snack) {
    if (!snack) {
        if (st_.hunger >= 4) {
            SetMessage("No tiene hambre", 12);
            return;
        }
        st_.hunger++;
        if (st_.weight < 99) st_.weight++;
        SetMessage("Nam nam!", 8);
    } else {
        if (st_.happy >= 4) {
            SetMessage("Ya esta feliz", 12);
            return;
        }
        st_.happy++;
        st_.weight = (uint8_t)std::min(99, st_.weight + 2);
        SetMessage("Que rico!", 8);
    }
    eat_frames_ = 10;
    ShowPopup(false);
    UpdateCall();
    Save();
}

void Tamagotchi::HandleButton(int id) {
    Activity();
    if (!st_.alive) return;
    if (st_.stage == 0) {
        SetMessage("Aun es un huevo...", 12);
        RefreshUI();
        return;
    }

    if (id == 20 || id == 21) {
        OnGamePick(id - 20);
        RefreshUI();
        return;
    }
    if (id >= 30 && id <= 39) {  // toques dentro de los minijuegos nuevos
        OnMiniTap(id - 30);
        return;
    }
    if (game_active_) {
        if (id == 1) EndGame(false);  // pulsar JUEGO otra vez = salir
        RefreshUI();
        return;
    }

    if (id == 2) {  // luz
        st_.lights_off = st_.lights_off ? 0 : 1;
        SetMessage(st_.lights_off ? "Luz apagada" : "Luz encendida", 10);
        Save();
        RefreshUI();
        return;
    }

    if (st_.asleep) {
        ShowPopup(false);
        SetMessage("Duerme... usa LUZ", 12);
        RefreshUI();
        return;
    }

    switch (id) {
        case 0:
            ShowPopup(!(popup_ && !lv_obj_has_flag(popup_, LV_OBJ_FLAG_HIDDEN)));
            break;
        case 1:
            ShowGamePopup(!(gpopup_ && !lv_obj_has_flag(gpopup_, LV_OBJ_FLAG_HIDDEN)));
            break;
        case 40:
            StartCatch();
            break;
        case 41:
            StartBag();
            break;
        case 42:
            StartMemo();
            break;
        case 43:
            StartGame();
            break;
        case 3:  // medicina
            if (!st_.sick) {
                SetMessage("No esta enfermo", 12);
            } else {
                st_.meds++;
                if (st_.meds >= 2) {
                    st_.sick = 0;
                    st_.meds = 0;
                    st_.sick_sec = 0;
                    SetMessage("Se curo!", 12);
                } else {
                    SetMessage("Falta otra dosis", 12);
                }
                UpdateCall();
                Save();
            }
            break;
        case 4:  // limpiar
            if (st_.poop == 0) {
                SetMessage("Ya esta limpio", 12);
            } else {
                st_.poop = 0;
                SetMessage("Limpio!", 10);
                Save();
            }
            break;
        case 5:  // disciplina
            if (st_.call == 2) {
                st_.discipline = (uint8_t)std::min(100, st_.discipline + 25);
                st_.call = 0;
                st_.call_sec = 0;
                SetMessage("Aprendio la leccion", 12);
                Save();
            } else {
                SetMessage("No hace falta", 10);
            }
            break;
        case 10:
            Feed(false);
            break;
        case 11:
            Feed(true);
            break;
        default:
            break;
    }
    RefreshUI();
}

// ---------------------------------------------------------------------------
// Minijuego: hacia donde mira?
// ---------------------------------------------------------------------------

void Tamagotchi::StartGame() {
    BeginMini(1);
    game_round_ = 0;
    game_wins_ = 0;
    Show(game_, true);
    NextRound();
}

void Tamagotchi::NextRound() {
    game_target_ = (int)Rnd(2);
    game_awaiting_ = true;
    look_ = 0;
    snprintf(game_text_, sizeof(game_text_), "Ronda %d/5 - Hacia donde mira? Toca IZQ o DER",
             game_round_ + 1);
}

void Tamagotchi::OnGamePick(int side) {
    if (!game_active_ || !game_awaiting_) return;
    game_awaiting_ = false;
    const bool hit = (side == game_target_);
    if (hit) game_wins_++;
    game_round_++;
    look_ = game_target_ ? 1 : -1;
    if (hit) {
        snprintf(game_text_, sizeof(game_text_), "Acierto!");
    } else {
        snprintf(game_text_, sizeof(game_text_), "Fallo! Miraba a la %s",
                 game_target_ ? "DER" : "IZQ");
    }
    game_wait_frames_ = 6;
}

void Tamagotchi::GameTick() {
    if (!game_active_) return;
    if (!st_.alive) {
        EndGame(false);
        return;
    }
    if (!game_awaiting_ && game_wait_frames_ > 0) {
        if (--game_wait_frames_ == 0) {
            look_ = 0;
            if (game_round_ >= 5) {
                EndGame(true);
            } else {
                NextRound();
            }
        }
    }
}

void Tamagotchi::EndGame(bool finished) {
    game_active_ = false;
    game_awaiting_ = false;
    game_type_ = 0;
    target_ = nullptr;
    for (int i = 0; i < 4; i++) pads_[i] = nullptr;
    look_ = 0;
    Show(game_, false);
    if (game_area_) {
        lv_obj_clean(game_area_);
        Show(game_area_, false);
    }
    Show(cont_, true);
    if (!finished) return;
    char buf[64];
    if (game_wins_ >= 3) {
        if (st_.happy < 4) st_.happy++;
        st_.weight = st_.weight > 5 ? st_.weight - 1 : 5;
        snprintf(buf, sizeof(buf), "Ganaste %d/5! Esta feliz", game_wins_);
    } else {
        snprintf(buf, sizeof(buf), "Perdiste (%d/5). Otra vez?", game_wins_);
    }
    SetMessage(buf, 16);
    UpdateCall();
    Save();
}

// ---------------------------------------------------------------------------
// Minijuegos: ATRAPA (bolas), SACO (golpes rapidos) y MEMO (colores)
// ---------------------------------------------------------------------------

void Tamagotchi::BeginMini(int type) {
    ShowPopup(false);
    ShowGamePopup(false);
    game_active_ = true;
    game_awaiting_ = false;
    game_wait_frames_ = 0;
    game_type_ = type;
    game_score_ = 0;
    target_ = nullptr;
    for (int i = 0; i < 4; i++) pads_[i] = nullptr;
    lv_obj_clean(game_area_);
    Show(game_area_, true);
    Show(game_, false);
    Show(cont_, type != 3 && type != 4);  // saco y memoria ocupan el centro
}

void Tamagotchi::StartCatch() {
    BeginMini(2);
    game_frames_ = 80;  // 20 segundos
    catch_life_ = 0;
    snprintf(game_text_, sizeof(game_text_), "ATRAPA las bolas antes de que desaparezcan!");
}

void Tamagotchi::StartBag() {
    BeginMini(3);
    game_frames_ = 40;  // 10 segundos
    bag_pulse_ = 0;
    target_ = MakeTouch(game_area_, 155, 40, 170, 170, 0x8B5A2B, LV_RADIUS_CIRCLE, 31);
    snprintf(game_text_, sizeof(game_text_), "SACO! Toca lo mas rapido que puedas");
}

void Tamagotchi::StartMemo() {
    BeginMini(4);
    const uint32_t cols[4] = {0xFF4D4D, 0x4D8DFF, 0x3DDC84, 0xFFD93D};
    const int px[4] = {95, 255, 95, 255};
    const int py[4] = {10, 10, 125, 125};
    for (int i = 0; i < 4; i++) {
        pads_[i] = MakeTouch(game_area_, px[i], py[i], 130, 105, cols[i], 24, 32 + i);
    }
    SetPads(-1);
    for (int i = 0; i < 8; i++) memo_seq_[i] = (uint8_t)Rnd(4);
    memo_len_ = 2;
    memo_phase_ = 0;
    memo_timer_ = -4;
    memo_input_ = 0;
    memo_flash_ = 0;
    memo_ok_ = false;
    snprintf(game_text_, sizeof(game_text_), "MEMO! Memoriza la secuencia de colores");
}

void Tamagotchi::SetPads(int lit) {
    for (int i = 0; i < 4; i++) {
        if (pads_[i]) lv_obj_set_style_bg_opa(pads_[i], i == lit ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

void Tamagotchi::OnMiniTap(int k) {
    if (!game_active_) return;
    if (game_type_ == 2 && k == 0) {
        if (!target_) return;
        game_score_++;
        lv_obj_delete_async(target_);
        target_ = nullptr;
        catch_life_ = 0;
    } else if (game_type_ == 3 && k == 1) {
        game_score_++;
        bag_pulse_ = 1;
    } else if (game_type_ == 4 && k >= 2 && k <= 5 && memo_phase_ == 1) {
        const int pad = k - 2;
        SetPads(pad);
        memo_flash_ = 2;
        if (pad == memo_seq_[memo_input_]) {
            memo_input_++;
            if (memo_input_ >= memo_len_) {
                memo_ok_ = true;
                memo_phase_ = 2;
                memo_timer_ = 4;
                snprintf(game_text_, sizeof(game_text_), "Muy bien!");
            }
        } else {
            memo_ok_ = false;
            memo_phase_ = 2;
            memo_timer_ = 4;
            snprintf(game_text_, sizeof(game_text_), "Fallaste!");
        }
    }
}

void Tamagotchi::MiniTick() {
    if (!game_active_ || game_type_ < 2) return;
    if (!st_.alive) {
        EndGame(false);
        return;
    }
    switch (game_type_) {
        case 2: {  // ATRAPA
            if (--game_frames_ <= 0) {
                FinishMini(game_score_ >= 8);
                return;
            }
            if (target_) {
                if (--catch_life_ <= 0) {
                    lv_obj_delete(target_);
                    target_ = nullptr;
                }
            } else {
                static const uint32_t cols[5] = {0xFF6B6B, 0xFECA57, 0x54A0FF, 0x1DD1A1, 0xA29BFE};
                target_ = MakeTouch(game_area_, 30 + (int)Rnd(356), 10 + (int)Rnd(170), 64, 64,
                                    cols[Rnd(5)], LV_RADIUS_CIRCLE, 30);
                catch_life_ = std::max(3, 7 - game_score_ / 4);
            }
            snprintf(game_text_, sizeof(game_text_), "ATRAPA! Tiempo %d  Puntos %d (meta 8)",
                     (game_frames_ + 3) / 4, game_score_);
            break;
        }
        case 3: {  // SACO
            if (--game_frames_ <= 0) {
                FinishMini(game_score_ >= 25);
                return;
            }
            if (target_) {
                const int d = 170 + 16 * bag_pulse_;
                lv_obj_set_size(target_, d, d);
                lv_obj_set_pos(target_, 240 - d / 2, 125 - d / 2);
                bag_pulse_ = 0;
            }
            snprintf(game_text_, sizeof(game_text_), "SACO! Tiempo %d  Golpes %d (meta 25)",
                     (game_frames_ + 3) / 4, game_score_);
            break;
        }
        case 4: {  // MEMO
            if (memo_phase_ == 0) {  // muestra la secuencia
                if (memo_timer_ < 0) {
                    memo_timer_++;
                    break;
                }
                const int step = memo_timer_ / 4;
                const int sub = memo_timer_ % 4;
                if (step >= memo_len_) {
                    SetPads(-1);
                    memo_phase_ = 1;
                    memo_input_ = 0;
                    snprintf(game_text_, sizeof(game_text_), "Tu turno! Repite la secuencia (%d)",
                             memo_len_);
                } else {
                    SetPads(sub < 2 ? memo_seq_[step] : -1);
                    memo_timer_++;
                }
            } else if (memo_phase_ == 1) {  // el jugador repite
                if (memo_flash_ > 0 && --memo_flash_ == 0) SetPads(-1);
            } else {  // resultado de la ronda
                if (memo_flash_ > 0 && --memo_flash_ == 0) SetPads(-1);
                if (--memo_timer_ <= 0) {
                    if (!memo_ok_) {
                        game_score_ = memo_len_ - 1;
                        FinishMini(false);
                        return;
                    }
                    if (memo_len_ >= 5) {
                        game_score_ = memo_len_;
                        FinishMini(true);
                        return;
                    }
                    memo_len_++;
                    memo_phase_ = 0;
                    memo_timer_ = -4;
                    snprintf(game_text_, sizeof(game_text_), "Bien! Mira la siguiente...");
                }
            }
            break;
        }
        default:
            break;
    }
}

void Tamagotchi::FinishMini(bool win) {
    const int type = game_type_;
    const int score = game_score_;
    EndGame(false);
    char buf[64];
    if (win) {
        if (st_.happy < 4) st_.happy++;
        if (type == 3) {
            st_.weight = st_.weight > 7 ? st_.weight - 2 : 5;
        } else if (st_.weight > 5) {
            st_.weight--;
        }
        if (type == 4) st_.discipline = (uint8_t)std::min(100, st_.discipline + 10);
        snprintf(buf, sizeof(buf), "Ganaste! (%d) Esta feliz", score);
    } else {
        snprintf(buf, sizeof(buf), "Perdiste (%d). Otra vez?", score);
    }
    SetMessage(buf, 16);
    UpdateCall();
    Save();
}

// ---------------------------------------------------------------------------
// Bucle principal (cada 250 ms, dentro del hilo de LVGL)
// ---------------------------------------------------------------------------

void Tamagotchi::Tick() {
    if (!root_) return;
    frame_++;

    const int64_t now_s = esp_timer_get_time() / 1000000;
    if (now_s > last_s_) {
        const uint32_t real = (uint32_t)(now_s - last_s_);
        last_s_ = now_s;
        TryCatchUp();
        uint32_t dt = real * TAMA_TIME_SCALE;
        while (dt > 0) {
            const uint32_t c = dt > 60 ? 60 : dt;
            Step(c);
            dt -= c;
        }
        since_save_ += real;
        if (since_save_ >= kSaveEverySec) {
            since_save_ = 0;
            Save();
        }
    }

    blink_ = (frame_ % 20) == 0;
    if (eat_frames_ > 0) eat_frames_--;
    if (msg_frames_ > 0) msg_frames_--;
    if (popup_frames_ > 0 && --popup_frames_ == 0) {
        Show(popup_, false);
        Show(gpopup_, false);
    }
    GameTick();
    MiniTick();

    // Punto rojo parpadeante cuando la mascota necesita algo y estas en modo asistente
    const bool attention = !st_.alive || st_.call != 0 || notify_;
    Show(alert_, !visible_ && attention && ((frame_ / 2) % 2 == 0));

    if (!visible_) return;

    int ox = 0, oy = 0;
    if (st_.alive) {
        if (st_.stage == 0) {
            ox = ((frame_ / 2) % 2) ? 4 : -4;
        } else if (st_.asleep) {
            oy = ((frame_ / 6) % 2) ? -2 : 0;
        } else {
            oy = ((frame_ / 2) % 2) ? -6 : 0;
        }
    }
    lv_obj_set_pos(cont_, kContX + ox, kContY + oy);
    RefreshUI();
}

// ---------------------------------------------------------------------------
// Dibujo
// ---------------------------------------------------------------------------

bool Tamagotchi::EggCracked() const { return st_.stage_sec * 10 > kEggSec * 6; }

uint32_t Tamagotchi::SpriteKey() const {
    return (st_.alive ? 1u : 0u) | ((uint32_t)st_.stage << 1) | ((uint32_t)st_.species << 4) |
           ((uint32_t)st_.sick << 8) | ((uint32_t)st_.adult_good << 9) |
           ((uint32_t)(EggCracked() ? 1 : 0) << 10);
}

const char* Tamagotchi::StatusText() const {
    if (!st_.alive) return "Toca la pantalla para un huevo nuevo";
    if (st_.stage == 0) return "Algo se mueve dentro del huevo...";
    if (st_.asleep) return st_.lights_off ? "Zzz... durmiendo" : "Duerme! Apaga la luz (LUZ)";
    if (st_.sick) return "Esta enfermo! Usa MEDI (2 veces)";
    if (st_.call == 2) return "Se porta mal! Usa DISC";
    if (st_.hunger == 0) return "Tiene mucha hambre!";
    if (st_.happy == 0) return "Esta triste! Juega con el";
    if (st_.poop > 0) return "Hay caca! Usa LIMP";
    return "";
}

void Tamagotchi::BuildUI() {
    root_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, kW, kH);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, C(0x000000), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(root_, OnRootClick, LV_EVENT_CLICKED, nullptr);

    // Corazones
    MakeLabel(root_, "HAMBRE", 0xFF9F43, LV_ALIGN_TOP_LEFT, 70, 14);
    MakeLabel(root_, "FELIZ", 0xFF5C8D, LV_ALIGN_TOP_LEFT, 290, 14);
    for (int i = 0; i < 4; i++) {
        hunger_dot_[i] = MakeDot(root_, 70 + i * 26, 34, 0xFF9F43);
        happy_dot_[i] = MakeDot(root_, 290 + i * 26, 34, 0xFF5C8D);
    }

    name_lbl_ = MakeLabel(root_, "", 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 60, 380);
    stats_lbl_ = MakeLabel(root_, "", 0xBBBBBB, LV_ALIGN_TOP_MID, 0, 80, 380);

    // Zona de la mascota
    cont_ = Box(root_, kContX, kContY, kContW, kContH, C(0x000000), 0);
    lv_obj_set_style_bg_opa(cont_, LV_OPA_TRANSP, 0);

    const int px[4] = {385, 425, 385, 425};
    const int py[4] = {300, 300, 264, 264};
    for (int i = 0; i < 4; i++) {
        poop_[i][0] = Circle(root_, px[i], py[i], 28, C(0x8B5A2B));
        poop_[i][1] = Circle(root_, px[i] + 5, py[i] - 12, 18, C(0x8B5A2B));
        Show(poop_[i][0], false);
        Show(poop_[i][1], false);
    }

    dark_ = Box(root_, 0, 92, kW, 300, C(0x000000), 0);
    lv_obj_set_style_bg_opa(dark_, LV_OPA_80, 0);
    Show(dark_, false);

    zzz_ = MakeLabel(root_, "Z z z", 0x9DB4FF, LV_ALIGN_TOP_MID, 100, 110);
    Show(zzz_, false);

    msg_lbl_ = MakeLabel(root_, "", 0xFFE066, LV_ALIGN_TOP_MID, 0, 350, 380);

    // Menu de comida
    popup_ = Box(root_, 60, 346, 360, 42, C(0x1E1E2E), 14);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    MakeButton(popup_, 6, 5, 170, 32, 0xFF9F43, 12, "COMIDA", 10);
    MakeButton(popup_, 184, 5, 170, 32, 0xFF6B9D, 12, "SNACK", 11);
    Show(popup_, false);

    // Botones inferiores
    for (int i = 0; i < kBtnCount; i++) {
        MakeButton(root_, 47 + i * 66, 396, 56, 56, kBtns[i].color, LV_RADIUS_CIRCLE,
                   kBtns[i].text, i);
    }

    // Botones del minijuego (a los lados de la mascota)
    game_ = Box(root_, 0, 96, kW, 290, C(0x000000), 0);
    lv_obj_set_style_bg_opa(game_, LV_OPA_TRANSP, 0);
    MakeButton(game_, 30, 60, 100, 170, 0x54A0FF, 28, "< IZQ", 20);
    MakeButton(game_, 350, 60, 100, 170, 0xFF6B6B, 28, "DER >", 21);
    Show(game_, false);

    // Menu para elegir minijuego
    gpopup_ = Box(root_, 20, 346, 440, 42, C(0x1E1E2E), 14);
    lv_obj_add_flag(gpopup_, LV_OBJ_FLAG_CLICKABLE);
    MakeButton(gpopup_, 6, 5, 104, 32, 0x54A0FF, 12, "ATRAPA", 40);
    MakeButton(gpopup_, 114, 5, 104, 32, 0xFF6B6B, 12, "SACO", 41);
    MakeButton(gpopup_, 222, 5, 104, 32, 0x1DD1A1, 12, "MEMO", 42);
    MakeButton(gpopup_, 330, 5, 104, 32, 0xA29BFE, 12, "MIRA", 43);
    Show(gpopup_, false);

    // Zona donde se dibujan los minijuegos nuevos
    game_area_ = Box(root_, 0, 96, kW, 250, C(0x000000), 0);
    lv_obj_set_style_bg_opa(game_area_, LV_OPA_TRANSP, 0);
    Show(game_area_, false);

    // Aviso en modo asistente
    alert_ = Circle(lv_layer_top(), 232, 456, 16, C(0xFF3B30));
    Show(alert_, false);
}

void Tamagotchi::BuildEgg() {
    const int w = 110, h = 140;
    const int x = kContW / 2 - w / 2;
    const int y = kBottom - h;
    Box(cont_, x, y, w, h, C(0xFFF4E0), LV_RADIUS_CIRCLE);
    Circle(cont_, x + 22, y + 40, 24, C(0xA0D8F0));
    Circle(cont_, x + 62, y + 66, 18, C(0xF0A0C8));
    Circle(cont_, x + 36, y + 96, 16, C(0xB8E0A0));
    if (EggCracked()) {
        Box(cont_, x + 30, y + 14, 22, 4, C(0x444444), 2);
        Box(cont_, x + 48, y + 20, 22, 4, C(0x444444), 2);
        Box(cont_, x + 66, y + 14, 18, 4, C(0x444444), 2);
    }
}

void Tamagotchi::BuildTomb() {
    const int w = 130;
    const int x = (kContW - w) / 2;
    const lv_color_t stone = C(0x9AA0A6);
    Box(cont_, x, kBottom - 170, w, w, stone, LV_RADIUS_CIRCLE);
    Box(cont_, x, kBottom - 170 + w / 2, w, 170 - w / 2, stone, 0);
    Box(cont_, kContW / 2 - 4, kBottom - 150, 8, 40, C(0x5C6066), 2);
    Box(cont_, kContW / 2 - 16, kBottom - 138, 32, 8, C(0x5C6066), 2);
    MakeLabel(cont_, "R.I.P.", 0x3A3D42, LV_ALIGN_TOP_MID, 0, kBottom - 82);
    Box(cont_, x - 20, kBottom - 6, w + 40, 8, C(0x3E8E41), 4);
}

void Tamagotchi::BuildSprite() {
    lv_obj_clean(cont_);
    eye_[0] = eye_[1] = nullptr;
    pup_[0] = pup_[1] = nullptr;
    mouth_ = nullptr;
    sprite_key_ = SpriteKey();

    if (!st_.alive) {
        BuildTomb();
        return;
    }
    if (st_.stage == 0) {
        BuildEgg();
        return;
    }

    const int f = st_.species % kNumSpecies;
    const SpeciesInfo& sp = kSpecies[f];
    const int s = (st_.stage == 1) ? 90 : (st_.stage == 2) ? 130 : (st_.stage == 3) ? 170 : 150;
    auto P = [s](float k) { return (int)(s * k); };

    lv_color_t body = C(sp.body);
    const lv_color_t belly = C(sp.belly);
    const lv_color_t accent = C(sp.accent);
    if (st_.sick) {
        body = lv_color_mix(C(0x7CCF3A), body, 110);
    } else if (st_.stage == 4) {
        body = lv_color_mix(C(0x9AA0A6), body, 90);
    } else if (st_.stage == 3 && !st_.adult_good) {
        body = lv_color_mix(C(0x2B2B4A), body, 70);
    }

    const int bw = s;
    const int bh = (f == 7) ? P(1.10f) : P(0.92f);
    const int lift = (f == 7) ? P(0.18f) : 0;
    const int by = kBottom - bh - lift;
    const int cx = kContW / 2;

    // --- Partes que van detras del cuerpo ---
    switch (f) {
        case 0: {  // gato: orejas
            const int d = P(0.34f);
            for (int side = -1; side <= 1; side += 2) {
                const int ex = cx + side * P(0.32f) - d / 2;
                const int ey = by - d / 2 + P(0.06f);
                Circle(cont_, ex, ey, d, body);
                Circle(cont_, ex + d / 4, ey + d / 4, d / 2, accent);
            }
            break;
        }
        case 1: {  // rana: bultos de los ojos
            const int d = P(0.36f);
            for (int side = -1; side <= 1; side += 2) {
                Circle(cont_, cx + side * P(0.27f) - d / 2, by - d / 2 + P(0.06f), d, body);
            }
            break;
        }
        case 2: {  // pollito: alitas
            const int w = P(0.18f), h = P(0.30f);
            const lv_color_t wing = lv_color_mix(accent, body, 90);
            Box(cont_, cx - bw / 2 - w / 2 + P(0.03f), by + (int)(bh * 0.45f), w, h, wing,
                LV_RADIUS_CIRCLE);
            Box(cont_, cx + bw / 2 - w / 2 - P(0.03f), by + (int)(bh * 0.45f), w, h, wing,
                LV_RADIUS_CIRCLE);
            break;
        }
        case 3: {  // oso: orejas redondas
            const int d = P(0.30f);
            for (int side = -1; side <= 1; side += 2) {
                const int ex = cx + side * P(0.34f) - d / 2;
                const int ey = by - d / 2 + P(0.08f);
                Circle(cont_, ex, ey, d, body);
                Circle(cont_, ex + d / 4, ey + d / 4, d / 2, belly);
            }
            break;
        }
        case 4: {  // alien: antenas
            const int d = P(0.13f);
            for (int side = -1; side <= 1; side += 2) {
                const int ax = cx + side * P(0.20f);
                Box(cont_, ax - 2, by - P(0.20f), 4, P(0.26f), accent, 2);
                Circle(cont_, ax - d / 2, by - P(0.20f) - d / 2, d, accent);
            }
            break;
        }
        case 5: {  // conejo: orejas largas
            const int w = P(0.20f), h = P(0.55f);
            for (int side = -1; side <= 1; side += 2) {
                const int ex = cx + side * P(0.20f) - w / 2;
                const int ey = by - h + P(0.12f);
                Box(cont_, ex, ey, w, h, body, LV_RADIUS_CIRCLE);
                Box(cont_, ex + w / 4, ey + (int)(h * 0.15f), w / 2, (int)(h * 0.65f), accent,
                    LV_RADIUS_CIRCLE);
            }
            break;
        }
        case 6: {  // dino: cola y pinchos
            Box(cont_, cx + P(0.38f), by + (int)(bh * 0.62f), P(0.30f), P(0.18f), body,
                LV_RADIUS_CIRCLE);
            const int d = P(0.14f);
            Circle(cont_, cx - P(0.24f) - d / 2, by - d / 2 + P(0.05f), d, accent);
            Circle(cont_, cx - d / 2, by - d * 6 / 10, d, accent);
            Circle(cont_, cx + P(0.24f) - d / 2, by - d / 2 + P(0.05f), d, accent);
            break;
        }
        default:
            break;
    }

    // --- Cuerpo ---
    Box(cont_, cx - bw / 2, by, bw, bh, body, LV_RADIUS_CIRCLE);
    if (f != 7) {
        Box(cont_, cx - P(0.29f), by + (int)(bh * 0.66f), P(0.58f), (int)(bh * 0.28f), belly,
            LV_RADIUS_CIRCLE);
    } else {  // fantasma: base ondulada
        const int d = s / 3;
        for (int i = 0; i < 3; i++) {
            Circle(cont_, cx - s / 2 + i * d, by + bh - d / 2, d, body);
        }
    }

    // Mejillas
    const int cd = std::max(8, P(0.13f));
    for (int side = -1; side <= 1; side += 2) {
        lv_obj_t* ch =
            Circle(cont_, cx + side * P(0.36f) - cd / 2, by + (int)(bh * 0.47f), cd, C(0xFF7A9C));
        lv_obj_set_style_bg_opa(ch, LV_OPA_50, 0);
    }

    // Ojos
    ed_ = std::max(12, P(0.20f));
    pd_ = ed_ / 2;
    eye_y_ = by + (int)(bh * 0.28f);
    eye_x_[0] = cx - P(0.22f) - ed_ / 2;
    eye_x_[1] = cx + P(0.22f) - ed_ / 2;
    for (int i = 0; i < 2; i++) {
        eye_[i] = Circle(cont_, eye_x_[i], eye_y_, ed_, C(0xFFFFFF));
        pup_[i] = Circle(cont_, eye_x_[i] + (ed_ - pd_) / 2, eye_y_ + (ed_ - pd_) / 2, pd_,
                         C(0x111111));
    }
    if (st_.stage == 3 && !st_.adult_good) {  // adulto "malo": ceno fruncido
        for (int i = 0; i < 2; i++) {
            Box(cont_, eye_x_[i] - 2, eye_y_ - 7, ed_ + 4, 4, C(0x222222), 2);
        }
    }

    // Boca
    face_cx_ = cx;
    mouth_y_ = by + (int)(bh * 0.56f);
    bw_ = bw;
    bh_ = bh;
    mouth_ = Circle(cont_, cx - 5, mouth_y_, 10, C(0x4A1F1F));

    // --- Partes que van delante ---
    switch (f) {
        case 0: {  // gato: naricita
            const int d = std::max(6, P(0.07f));
            Circle(cont_, cx - d / 2, by + (int)(bh * 0.43f), d, C(0xFF5C8D));
            break;
        }
        case 2: {  // pollito: pico y cresta
            const int w = P(0.22f), h = P(0.12f);
            Box(cont_, cx - w / 2, by + (int)(bh * 0.41f), w, h, accent, LV_RADIUS_CIRCLE);
            const int d = P(0.10f);
            Circle(cont_, cx - P(0.09f) - d / 2, by - d / 2, d, C(0xFF3B30));
            Circle(cont_, cx - d / 2, by - d * 8 / 10, d, C(0xFF3B30));
            Circle(cont_, cx + P(0.09f) - d / 2, by - d / 2, d, C(0xFF3B30));
            break;
        }
        case 3: {  // oso: nariz
            const int d = P(0.12f);
            Circle(cont_, cx - d / 2, by + (int)(bh * 0.42f), d, C(0x3A2418));
            break;
        }
        default:
            break;
    }

    if (st_.stage == 4) {  // anciano: baston
        Box(cont_, cx + P(0.58f), by + (int)(bh * 0.45f), 6, (int)(bh * 0.55f), C(0x8B5A2B), 3);
    }
}

void Tamagotchi::UpdateFace() {
    if (!eye_[0] || !mouth_) return;
    const bool closed = st_.asleep || blink_;
    for (int i = 0; i < 2; i++) {
        if (closed) {
            lv_obj_set_size(eye_[i], ed_, 5);
            lv_obj_set_pos(eye_[i], eye_x_[i], eye_y_ + ed_ / 2 - 2);
            lv_obj_set_style_bg_color(eye_[i], C(0x222222), 0);
            Show(pup_[i], false);
        } else {
            lv_obj_set_size(eye_[i], ed_, ed_);
            lv_obj_set_pos(eye_[i], eye_x_[i], eye_y_);
            lv_obj_set_style_bg_color(eye_[i], C(0xFFFFFF), 0);
            Show(pup_[i], true);
            lv_obj_set_pos(pup_[i], eye_x_[i] + (ed_ - pd_) / 2 + look_ * (ed_ - pd_) / 2,
                           eye_y_ + (ed_ - pd_) / 2);
        }
    }

    const bool sad = st_.hunger == 0 || st_.happy == 0 || st_.sick;
    const bool happy = st_.hunger >= 3 && st_.happy >= 3;
    int w, h, dy = 0;
    if (st_.asleep) {
        w = bw_ / 10;
        h = bh_ / 25;
    } else if (eat_frames_ > 0) {
        w = h = bw_ * 18 / 100 + (eat_frames_ % 2) * 4;
    } else if (sad) {
        w = bw_ * 12 / 100;
        h = bh_ * 7 / 100;
        dy = 8;
    } else if (happy) {
        w = bw_ * 30 / 100;
        h = bh_ * 15 / 100;
    } else {
        w = bw_ * 20 / 100;
        h = bh_ * 5 / 100;
    }
    w = std::max(w, 6);
    h = std::max(h, 4);
    lv_obj_set_size(mouth_, w, h);
    lv_obj_set_pos(mouth_, face_cx_ - w / 2, mouth_y_ + dy);
}

void Tamagotchi::RefreshUI() {
    if (!root_) return;
    if (SpriteKey() != sprite_key_) BuildSprite();
    UpdateFace();

    const bool live = st_.alive && st_.stage > 0;
    for (int i = 0; i < 4; i++) {
        SetDot(hunger_dot_[i], live && i < st_.hunger);
        SetDot(happy_dot_[i], live && i < st_.happy);
    }

    char name[64];
    char stats[96];
    const SpeciesInfo& sp = kSpecies[st_.species % kNumSpecies];
    if (!st_.alive) {
        snprintf(name, sizeof(name), "%s (R.I.P.)", sp.name);
        const char* cause = st_.death_cause == 0   ? "Murio de hambre y descuido"
                            : st_.death_cause == 1 ? "Murio enfermo"
                                                   : "Murio de viejo";
        snprintf(stats, sizeof(stats), "%s a los %ud", cause,
                 (unsigned)(st_.died_age_sec / kDaySec));
    } else if (st_.stage == 0) {
        snprintf(name, sizeof(name), "Huevo");
        snprintf(stats, sizeof(stats), "Va a nacer pronto");
    } else {
        snprintf(name, sizeof(name), "%s - %s", sp.name, kStageNames[st_.stage]);
        snprintf(stats, sizeof(stats), "Edad %ud %02uh   Peso %ug   Disc %u%%",
                 (unsigned)(st_.age_sec / kDaySec), (unsigned)((st_.age_sec % kDaySec) / 3600),
                 (unsigned)st_.weight, (unsigned)st_.discipline);
    }
    SetLabel(name_lbl_, name);
    SetLabel(stats_lbl_, stats);

    for (int i = 0; i < 4; i++) {
        const bool on = live && i < st_.poop;
        Show(poop_[i][0], on);
        Show(poop_[i][1], on);
    }

    Show(dark_, live && st_.lights_off);
    Show(zzz_, live && st_.asleep);

    const char* m = game_active_ ? game_text_ : (msg_frames_ > 0 ? msg_ : StatusText());
    SetLabel(msg_lbl_, m);
}
