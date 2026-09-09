#include "recording.h"

#include "mcap_recording.h"
// #include "lerobot_recording.h"  // added when the LeRobot reader lands

#include <cstdio>

namespace mp {

std::unique_ptr<Recording> open_recording(const std::string& path) {
    auto ends_with = [&](const char* suffix) {
        std::string s = suffix;
        return path.size() >= s.size() &&
               path.compare(path.size() - s.size(), s.size(), s) == 0;
    };

    if (ends_with(".mcap")) {
        auto rec = std::make_unique<McapRecording>();
        if (rec->open(path)) return rec;
        return nullptr;
    }

    // TODO: a directory containing meta/info.json → LeRobotRecording.

    std::fprintf(stderr, "open_recording: unrecognised format: %s\n", path.c_str());
    return nullptr;
}

} // namespace mp
