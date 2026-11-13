#include "radio.h"

#include <algorithm>

static const std::vector<RadioStation> kStations = {
    {"Los 40 Principales",
     "https://playerservices.streamtheworld.com/api/livestream-redirect/LOS40.mp3"},
    {"Kiss FM",
     "https://kissfm.kissfmradio.cires21.com/kissfm.mp3"},
    {"Gozadera FM",
     "https://streaming.shoutcast.com/gozadera"},
};

const std::vector<RadioStation>& RadioService::GetStations() {
    return kStations;
}

std::string RadioService::GetDefaultStationName() {
    return kStations.empty() ? std::string() : kStations.front().name;
}

std::string RadioService::GetDefaultStationUrl() {
    return kStations.empty() ? std::string() : kStations.front().url;
}

bool RadioService::Start(const std::string& url) {
    current_url_ = url;
    playing_ = true;
    return true;
}

bool RadioService::Stop() {
    current_url_.clear();
    playing_ = false;
    return true;
}

bool RadioService::IsPlaying() const {
    return playing_;
}

bool RadioService::SetVolume(int volume) {
    volume = std::max(0, std::min(100, volume));
    volume_ = volume;
    return true;
}

int RadioService::GetVolume() const {
    return volume_;
}
