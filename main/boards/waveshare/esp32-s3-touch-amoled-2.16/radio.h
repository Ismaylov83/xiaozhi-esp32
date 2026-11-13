#pragma once

#include <string>
#include <vector>

struct RadioStation {
    const char* name;
    const char* url;
};

class RadioService {
public:
    static const std::vector<RadioStation>& GetStations();
    static std::string GetDefaultStationName();
    static std::string GetDefaultStationUrl();

    bool Start(const std::string& url);
    bool Stop();
    bool IsPlaying() const;
    bool SetVolume(int volume);
    int GetVolume() const;

private:
    bool playing_ = true;
    int volume_ = 70;
    std::string current_url_;
};
