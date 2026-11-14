#include "radio.h"
#include "application.h"
#include "board.h"
#include "display/display.h"

#include <esp_http_client.h>
#if __has_include(<esp_crt_bundle.h>)
#include <esp_crt_bundle.h>
#define RADIO_HAVE_CRT_BUNDLE 1
#endif
#include <freertos/FreeRTOS.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>

#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdmmc_cmd.h>

extern "C" {
#include "mp3dec.h"
}

#include <algorithm>
#include <dirent.h>
#include <memory>
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
    {"Groove Salad", "ReggaeParis", "https://www.radio.es/s/nrjreggae"},
    {"Drone Zone", "ZonaRap Radio", "https://www.radio.es/s/zonarap"},
    {"Secret Agent", "AFN Rota", "https://www.radio.es/s/afn360rota"},
    {"Lush", "KISS FM", "https://tunein.com/radio/KISS-FM-(ESPA%C3%91A)-940-s6055/"},
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

void OnBarsTimer(lv_timer_t*) { Radio::GetInstance().BarsTick(); }

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

// ---------------------------------------------------------------------------
// Audio: altavoz de xiaozhi, internet / SD y decodificador MP3
// ---------------------------------------------------------------------------

std::atomic<int> g_gain{179};  // volumen de la radio, 0..256 (256 = sin cambio)
std::atomic<uint8_t> g_levels[16];  // niveles para las barras

// Altavoz de xiaozhi
class CodecOutput {
public:
    int Rate() { return Codec()->output_sample_rate(); }

    // pcm: mono a la frecuencia del codec
    void Write(std::vector<int16_t>& pcm) {
        const int gain = g_gain.load();
        if (gain != 256) {
            for (auto& s : pcm) s = (int16_t)(((int)s * gain) >> 8);
        }
        const size_t n = pcm.size();
        if (n >= 64) {  // niveles de 16 tramos para el visualizador
            const size_t seg = n / 16;
            for (int b = 0; b < 16; b++) {
                int peak = 0;
                for (size_t i = (size_t)b * seg; i < (size_t)(b + 1) * seg; i += 4) {
                    int v = pcm[i];
                    if (v < 0) v = -v;
                    if (v > peak) peak = v;
                }
                g_levels[b].store((uint8_t)std::min(255, peak >> 6));
            }
        }
        auto* codec = Codec();
        if (!codec->output_enabled()) codec->EnableOutput(true);
        if (codec->output_channels() == 2) {
            std::vector<int16_t> st(n * 2);
            for (size_t i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = pcm[i];
            codec->OutputData(st);
        } else {
            codec->OutputData(pcm);
        }
    }

private:
    static AudioCodec* Codec() { return Board::GetInstance().GetAudioCodec(); }
};

// Cliente HTTP. Tambien lee archivos de la SD (rutas /sdcard/...).
class RadioHttp {
public:
    ~RadioHttp() { Close(); }

    bool Open(const std::string& url) {
        Close();
        if (url.rfind("/sdcard/", 0) == 0 || url.rfind("file://", 0) == 0) {
            const std::string path = url.rfind("file://", 0) == 0 ? url.substr(7) : url;
            file_ = fopen(path.c_str(), "rb");
            return file_ != nullptr;
        }

        std::string cur = url;
        for (int hop = 0; hop < 5; hop++) {
            esp_http_client_config_t cfg = {};
            cfg.url = cur.c_str();
            cfg.timeout_ms = timeout_ms_;
            cfg.buffer_size = 2048;
            cfg.buffer_size_tx = 1024;
#ifdef RADIO_HAVE_CRT_BUNDLE
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
            h_ = esp_http_client_init(&cfg);
            if (!h_) return false;
            esp_http_client_set_method(h_, HTTP_METHOD_GET);
            esp_http_client_set_header(h_, "Icy-MetaData", "0");
            esp_http_client_set_header(h_, "User-Agent", "Mozilla/5.0 (ESP32 Radio)");
            if (esp_http_client_open(h_, 0) != ESP_OK) {
                Close();
                return false;
            }
            if (esp_http_client_fetch_headers(h_) < 0) {
                Close();
                return false;
            }
            status_ = esp_http_client_get_status_code(h_);
            if (status_ == 301 || status_ == 302 || status_ == 303 || status_ == 307 ||
                status_ == 308) {
                char* loc = nullptr;
                if (esp_http_client_get_header(h_, "Location", &loc) != ESP_OK || !loc) {
                    Close();
                    return false;
                }
                cur = loc;  // se copia antes de cerrar
                Close();
                continue;
            }
            return true;
        }
        return false;
    }

    bool IsFile() const { return file_ != nullptr; }
    int StatusCode() const { return file_ ? 200 : status_; }

    int Read(uint8_t* buf, size_t size) {
        if (file_) return (int)fread(buf, 1, size, file_);
        if (!h_) return -1;
        return esp_http_client_read(h_, (char*)buf, (int)size);
    }

    void Close() {
        if (file_) {
            fclose(file_);
            file_ = nullptr;
        }
        if (h_) {
            esp_http_client_close(h_);
            esp_http_client_cleanup(h_);
            h_ = nullptr;
        }
    }

    void SetTimeout(int ms) { timeout_ms_ = ms; }

private:
    esp_http_client_handle_t h_ = nullptr;
    FILE* file_ = nullptr;
    int status_ = 0;
    int timeout_ms_ = 10000;
};

CodecOutput g_output;

// Reproductor MP3 en streaming:
//   tarea 1: descarga (internet o archivo) -> anillo de 96 KB en PSRAM
//   tarea 2: anillo -> decodifica (Helix) -> mono -> frecuencia del codec -> altavoz
// Cada reproduccion es una "sesion" independiente: parar o cambiar de emisora no espera
// a nadie; las tareas viejas acaban solas y liberan su memoria.
class Mp3Stream {
public:
    std::function<void()> on_started;
    std::function<void(const char*)> on_error;
    std::function<void()> on_finished;

    bool Play(const std::string& url, std::string* err) {
        Stop();
        auto s = std::make_shared<Session>();
        s->owner = this;
        s->url = url;
        s->ring = xRingbufferCreateWithCaps(kRingSize, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
        if (!s->ring) {
            if (err) *err = "sin memoria";
            return false;
        }
        cur_ = s;
        if (xTaskCreate(DownloadEntry, "mp3_dl", 10240, new std::shared_ptr<Session>(s), 4,
                        nullptr) != pdPASS) {
            Stop();
            if (err) *err = "no se pudo crear la tarea de descarga";
            return false;
        }
        if (xTaskCreate(DecodeEntry, "mp3_dec", 10240, new std::shared_ptr<Session>(s), 6,
                        nullptr) != pdPASS) {
            Stop();
            if (err) *err = "no se pudo crear la tarea de audio";
            return false;
        }
        return true;
    }

    void Stop() {
        if (cur_) {
            cur_->silent = true;
            cur_->abort = true;
            cur_.reset();
        }
    }

private:
    static constexpr size_t kRingSize = 96 * 1024;
    static constexpr int kInBuf = 8192;

    struct Session {
        Mp3Stream* owner = nullptr;
        RingbufHandle_t ring = nullptr;
        std::string url;
        std::atomic<bool> abort{false};
        std::atomic<bool> silent{false};
        std::atomic<bool> done{false};
        std::atomic<bool> failed{false};
        ~Session() {
            if (ring) vRingbufferDeleteWithCaps(ring);
        }
    };

    void Fail(Session* s, const char* msg) {
        bool expected = false;
        if (s->failed.compare_exchange_strong(expected, true)) {
            s->abort = true;
            if (!s->silent && on_error) on_error(msg);
        }
    }

    static void DownloadEntry(void* arg) {
        auto* sp = static_cast<std::shared_ptr<Session>*>(arg);
        std::shared_ptr<Session> s = *sp;
        delete sp;
        s->owner->DownloadLoop(s.get());
        s.reset();
        vTaskDelete(nullptr);
    }

    static void DecodeEntry(void* arg) {
        auto* sp = static_cast<std::shared_ptr<Session>*>(arg);
        std::shared_ptr<Session> s = *sp;
        delete sp;
        s->owner->DecodeLoop(s.get());
        s.reset();
        vTaskDelete(nullptr);
    }

    void DownloadLoop(Session* s) {
        RadioHttp http;
        http.SetTimeout(8000);
        uint8_t* buf = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
        if (!buf) {
            Fail(s, "sin memoria");
            return;
        }
        if (!http.Open(s->url)) {
            Fail(s, "no se pudo conectar");
        } else if (http.StatusCode() != 200) {
            Fail(s, "el servidor no respondio bien");
        } else {
            int zeros = 0;
            while (!s->abort) {
                const int n = http.Read(buf, 2048);
                if (n < 0) {
                    Fail(s, "conexion perdida");
                    break;
                }
                if (n == 0) {
                    if (http.IsFile()) break;  // fin del archivo
                    if (++zeros > 40) {
                        Fail(s, "emision cortada");
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                zeros = 0;
                bool sent = false;
                while (!sent && !s->abort) {
                    sent = xRingbufferSend(s->ring, buf, (size_t)n, pdMS_TO_TICKS(200)) == pdTRUE;
                }
            }
            if (!s->failed) s->done = true;  // el flujo termino
        }
        heap_caps_free(buf);
    }

    void DecodeLoop(Session* s) {
        HMP3Decoder dec = MP3InitDecoder();
        uint8_t* in = (uint8_t*)heap_caps_malloc(kInBuf, MALLOC_CAP_SPIRAM);
        int16_t* pcm = (int16_t*)heap_caps_malloc(2304 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!dec || !in || !pcm) {
            Fail(s, "sin memoria para decodificar");
        } else {
            int bytes_in = 0;
            bool started = false;
            int bad = 0;
            float pos = 0.0f;
            int16_t last = 0;
            std::vector<int16_t> w, out;

            while (!s->abort) {
                bool ring_empty = false;
                if (bytes_in < kInBuf - 2048) {  // hay hueco: rellenar desde el anillo
                    size_t got = 0;
                    void* item = xRingbufferReceiveUpTo(s->ring, &got, pdMS_TO_TICKS(s->done ? 10 : 100),
                                                        (size_t)(kInBuf - bytes_in));
                    if (item) {
                        memcpy(in + bytes_in, item, got);
                        bytes_in += (int)got;
                        vRingbufferReturnItem(s->ring, item);
                    } else if (s->done) {
                        ring_empty = true;
                    }
                }
                if (ring_empty && bytes_in < 4) break;      // todo reproducido
                if (bytes_in < 1800 && !s->done) continue;  // falta un frame completo

                uint8_t* rp = in;
                int left = bytes_in;
                const int off = MP3FindSyncWord(rp, left);
                if (off < 0) {
                    bytes_in = 0;
                    continue;
                }
                rp += off;
                left -= off;
                uint8_t* frame_start = rp;

                const int err = MP3Decode(dec, &rp, &left, pcm, 0);
                if (err == ERR_MP3_NONE) {
                    bad = 0;
                    MP3FrameInfo fi;
                    MP3GetLastFrameInfo(dec, &fi);
                    if (fi.samprate > 0 && fi.nChans > 0 && fi.outputSamps > 0) {
                        const int ch = fi.nChans;
                        const int n = fi.outputSamps / ch;
                        // mono con una muestra extra al principio (la ultima del frame anterior)
                        w.resize((size_t)n + 1);
                        w[0] = last;
                        for (int i = 0; i < n; i++) {
                            w[(size_t)i + 1] =
                                ch == 2 ? (int16_t)(((int)pcm[2 * i] + (int)pcm[2 * i + 1]) / 2)
                                        : pcm[i];
                        }
                        // pasa a la frecuencia del codec (interpolacion lineal)
                        int dst = g_output.Rate();
                        if (dst <= 0) dst = 24000;
                        const float step = (float)fi.samprate / (float)dst;
                        out.clear();
                        while (pos < (float)n) {
                            const int i = (int)pos;
                            const float f = pos - (float)i;
                            out.push_back((int16_t)((float)w[i] + (float)(w[i + 1] - w[i]) * f));
                            pos += step;
                        }
                        pos -= (float)n;
                        last = w[(size_t)n];

                        if (!started) {
                            started = true;
                            if (!s->silent && on_started) on_started();
                        }
                        if (!out.empty()) g_output.Write(out);
                    }
                } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
                    if (ring_empty || s->done) break;       // no hay mas datos
                    if (bytes_in >= kInBuf - 2048) {        // sin hueco para rellenar: saltar un byte
                        rp = frame_start + 1;
                        left = (int)((in + bytes_in) - rp);
                    }
                } else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
                    // normal en los primeros frames: el siguiente se decodifica bien
                } else {
                    rp = frame_start + 1;  // dato corrupto: saltar un byte
                    left = (int)((in + bytes_in) - rp);
                    if (!started && ++bad > 400) {
                        Fail(s, "no es un MP3 valido");
                        break;
                    }
                }

                // quitar lo ya consumido
                if (left < 0) left = 0;
                memmove(in, rp, (size_t)left);
                bytes_in = left;
            }
            if (!s->abort && s->done && !s->silent && on_finished) on_finished();
        }
        if (dec) MP3FreeDecoder(dec);
        if (in) heap_caps_free(in);
        if (pcm) heap_caps_free(pcm);
        s->abort = true;  // que la descarga tambien termine
    }

    std::shared_ptr<Session> cur_;
};

Mp3Stream& Stream() {
    static Mp3Stream s;
    return s;
}

// Orden para la tarea que arranca y para la reproduccion (Play/Stop pueden tardar)
struct Cmd {
    int type;  // 1 = reproducir, 0 = parar
    char url[300];
    char title[64];
};

}  // namespace

// Lo lee audio_service.cc para no apagar el altavoz mientras suena la radio
std::atomic<bool> g_external_audio_active{false};

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
    cmd_q_ = xQueueCreate(4, sizeof(Cmd));
    xTaskCreate(CmdTask, "radio_cmd", 6144, nullptr, 5, nullptr);
    lv_timer_create(OnTimer, 1000, nullptr);
    lv_timer_create(OnBarsTimer, 120, nullptr);
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
    if (!root_) return;
    tick_count_++;

    // Control de la reproduccion (funciona aunque la radio no este en pantalla)
    if (playing_) {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            StopPlayback("Pausada (asistente)");  // el asistente habla o escucha
        } else if (reconnect_in_ > 0 && --reconnect_in_ == 0) {
            SetStatus("Conectando...");
            SendCmd(1, cur_source_, cur_title_);
        }
    }

    if (!visible_) return;
    RefreshClock();
    if (tick_count_ % 5 == 0) UpdateStatusIcons();
}

// ---------------------------------------------------------------------------
// Reproduccion de emisoras y archivos
// ---------------------------------------------------------------------------

void Radio::EnsurePlayer() {
    if (player_ready_) return;
    player_ready_ = true;

    Stream().on_started = [this]() {
        retries_ = 0;
        SetStatus(is_file_ ? "Reproduciendo archivo" : "Sonando");
    };
    Stream().on_error = [this](const char* message) {
        ESP_LOGW(TAG, "Error de audio: %s", message);
        if (playing_ && !is_file_ && retries_ < 5) {
            retries_++;
            reconnect_in_ = 3;
            SetStatus("Reconectando...");
        } else {
            playing_ = false;
            g_external_audio_active = false;
            SetStatus(message);
        }
    };
    Stream().on_finished = [this]() {
        if (!playing_) return;
        if (is_file_) {
            playing_ = false;
            g_external_audio_active = false;
            SetStatus("Fin del archivo");
        } else if (retries_ < 5) {
            retries_++;
            reconnect_in_ = 3;
            SetStatus("Reconectando...");
        } else {
            playing_ = false;
            g_external_audio_active = false;
            SetStatus("Emision cortada");
        }
    };
}

void Radio::CmdTask(void*) {
    Radio& r = Radio::GetInstance();
    Cmd c;
    for (;;) {
        if (xQueueReceive((QueueHandle_t)r.cmd_q_, &c, portMAX_DELAY) != pdTRUE) continue;
        r.EnsurePlayer();
        if (c.type == 1) {
            std::string err;
            if (!Stream().Play(c.url, &err)) {
                r.playing_ = false;
                g_external_audio_active = false;
                r.SetStatus(("Error: " + err).c_str());
            }
        } else {
            Stream().Stop();
            g_external_audio_active = false;
        }
    }
}

void Radio::SendCmd(int type, const std::string& url, const std::string& title) {
    if (!cmd_q_) return;
    Cmd c;
    memset(&c, 0, sizeof(c));
    c.type = type;
    snprintf(c.url, sizeof(c.url), "%s", url.c_str());
    snprintf(c.title, sizeof(c.title), "%s", title.c_str());
    xQueueSend((QueueHandle_t)cmd_q_, &c, 0);
}

void Radio::SetStatus(const char* text) {
    if (!display_) return;
    DisplayLockGuard lock(display_);
    status_ = text;
    if (np_status_) SetLabel(np_status_, status_.c_str());
}

void Radio::StartPlayback(const std::string& source, const std::string& title, bool is_file) {
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        SetStatus("Asistente ocupado");
        return;
    }
    cur_source_ = source;
    cur_title_ = title;
    is_file_ = is_file;
    retries_ = 0;
    reconnect_in_ = 0;
    playing_ = true;
    g_external_audio_active = true;
    SetStatus("Conectando...");
    SendCmd(1, source, title);
    RefreshNow();
}

void Radio::StopPlayback(const char* status) {
    playing_ = false;
    reconnect_in_ = 0;
    SendCmd(0);
    SetStatus(status);
    RefreshNow();
}

void Radio::StartStation() {
    if (selected_ < 0 || selected_ >= (int)stations_.size()) return;
    const Station& s = stations_[selected_];
    StartPlayback(s.url, s.name, false);
}

// Barras del visualizador: suben con el volumen del sonido y bajan poco a poco
void Radio::BarsTick() {
    if (!root_ || !visible_) return;
    for (int i = 0; i < 16; i++) {
        int target = 4;
        if (playing_) target = 4 + (int)g_levels[i].load() * 24 / 130;
        target = std::min(28, target);
        bar_h_[i] = target > bar_h_[i] ? target : std::max(4, bar_h_[i] - 3);
        lv_obj_set_size(bars_[i], 8, bar_h_[i]);
        lv_obj_set_y(bars_[i], 102 - bar_h_[i]);
    }
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

    // Archivo: los MP3 se reproducen; del resto solo se muestra la informacion
    const std::string full = sd_path_ + "/" + en.name;
    const char kind = FileKind(en.name);
    std::string lower = en.name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    const bool is_mp3 = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".mp3") == 0;

    char buf[200];
    if (is_mp3) {
        snprintf(buf, sizeof(buf), "Reproduciendo: %s", en.name.c_str());
        sd_msg_ = buf;
        SetLabel(sd_info_, sd_msg_.c_str());
        StartPlayback(full, en.name, true);
        return;
    }
    snprintf(buf, sizeof(buf), "%s - %s", en.name.c_str(),
             kind == 'A' ? "por ahora solo se reproduce MP3"
             : kind == 'V' ? "video: etapa 4" : "archivo no reproducible");
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
        bars_[i] = Box(np, 10 + i * 11, 98, 8, 4, C(0x2EA8F0), 2);
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
    g_gain = muted_ ? 0 : (volume_ * 256) / 100;
}

void Radio::RefreshNow() {
    if (playing_ && is_file_) {
        SetLabel(np_name_, cur_title_.c_str());
    } else if (selected_ >= 0 && selected_ < (int)stations_.size()) {
        SetLabel(np_name_, stations_[selected_].name.c_str());
    }
    SetLabel(np_status_, status_.empty() ? "Detenido" : status_.c_str());
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
        if (playing_ && !is_file_) StartStation();  // cambia de emisora al instante
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
            if (playing_) {
                StopPlayback("Detenido");
            } else {
                StartStation();
            }
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
