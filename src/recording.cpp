#include "recording.h"

#include "lerobot_recording.h"
#include "mcap_recording.h"

#include <cstdio>
#include <sys/stat.h>

namespace mp {

namespace {
bool is_dir(const std::string& p) {
    struct stat s;
    return ::stat(p.c_str(), &s) == 0 && (s.st_mode & S_IFDIR);
}
bool file_exists(const std::string& p) {
    struct stat s;
    return ::stat(p.c_str(), &s) == 0 && (s.st_mode & S_IFREG);
}
bool ends_with(const std::string& p, const char* suffix) {
    std::string s = suffix;
    return p.size() >= s.size() && p.compare(p.size() - s.size(), s.size(), s) == 0;
}
} // namespace

std::unique_ptr<Recording> open_recording(const std::string& path) {
    if (ends_with(path, ".mcap")) {
        auto rec = std::make_unique<McapRecording>();
        if (rec->open(path)) return rec;
        return nullptr;
    }

    // A directory holding meta/info.json -> a LeRobot dataset.
    if (is_dir(path) && file_exists(path + "/meta/info.json")) {
        auto rec = std::make_unique<LeRobotRecording>();
        if (rec->open(path)) return rec;
        return nullptr;
    }

    std::fprintf(stderr, "open_recording: unrecognised format: %s\n", path.c_str());
    return nullptr;
}

} // namespace mp
