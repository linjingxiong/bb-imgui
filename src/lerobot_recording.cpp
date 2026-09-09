#include "lerobot_recording.h"

#include "parquet.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace mp {

namespace {
// Format a "chunk-{c:03d}/file-{f:03d}" style relative path.
std::string data_parquet(const std::string& dir, int chunk, int file) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s/data/chunk-%03d/file-%03d.parquet", dir.c_str(), chunk,
                  file);
    return buf;
}
std::string video_mp4(const std::string& dir, const std::string& key, int chunk, int file) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s/videos/%s/chunk-%03d/file-%03d.mp4", dir.c_str(),
                  key.c_str(), chunk, file);
    return buf;
}
// SQL-quote a column name that contains '/' or '.'
std::string q(const std::string& name) { return "\"" + name + "\""; }
} // namespace

LeRobotRecording::~LeRobotRecording() { mp4_.clear(); }

bool LeRobotRecording::open(const std::string& dir) {
    dir_ = dir;
    // strip a trailing slash
    while (!dir_.empty() && (dir_.back() == '/' || dir_.back() == '\\')) dir_.pop_back();

    nlohmann::json info;
    {
        std::ifstream f(dir_ + "/meta/info.json");
        if (!f) { std::fprintf(stderr, "lerobot: no meta/info.json in %s\n", dir_.c_str()); return false; }
        try { f >> info; } catch (...) { std::fprintf(stderr, "lerobot: bad info.json\n"); return false; }
    }
    std::string ver;
    try {
        ver = info.value("codebase_version", "");
    } catch (...) {}
    if (ver.rfind("v3", 0) != 0) {
        std::fprintf(stderr, "lerobot: unsupported codebase_version '%s' (need v3.*)\n", ver.c_str());
        return false;
    }
    fps_ = info.value("fps", 30.0);

    // Classify channels from `features`.
    static const char* kIndexCols[] = {"timestamp",     "frame_index", "episode_index",
                                       "index",         "task_index",  "next.reward",
                                       "next.done",     "next.success"};
    try {
    for (auto it = info["features"].begin(); it != info["features"].end(); ++it) {
        const std::string name = it.key();
        const auto& feat = it.value();
        const std::string dtype = feat.value("dtype", "");
        auto shape = feat.value("shape", std::vector<int>{});
        if (dtype == "video") {
            VideoChannelInfo vi;
            vi.valid = true;
            vi.display_name = name;
            if (shape.size() >= 2) { vi.height = shape[0]; vi.width = shape[1]; }
            // the codec block is "video_info" in some datasets, "info" in others
            nlohmann::json vinf = feat.value("video_info", feat.value("info", nlohmann::json::object()));
            vi.codec = vinf.value("video.codec", "");
            vi.fps = vinf.value("video.fps", fps_);
            vinfo_[name] = vi;
            video_keys_.push_back(name);
        } else {
            bool is_index = false;
            for (auto* c : kIndexCols) if (name == c) is_index = true;
            if (is_index || !feat.contains("names") || feat["names"].is_null()) continue;
            int dims = shape.empty() ? 1 : shape[0];
            if (dims < 1) continue;
            // `names` may be ["a","b",…] or {"group":["a","b",…]}
            std::vector<std::string> labels;
            const auto& nm = feat["names"];
            if (nm.is_array()) {
                for (const auto& n : nm)
                    if (n.is_string()) labels.push_back(n.get<std::string>());
            } else if (nm.is_object()) {
                for (auto ni = nm.begin(); ni != nm.end(); ++ni)
                    if (ni.value().is_array())
                        for (const auto& n : ni.value())
                            if (n.is_string()) labels.push_back(n.get<std::string>());
            }
            ScalarChannelInfo si;
            si.valid = true;
            si.display_name = name;
            si.dims = dims;
            si.dim_labels = std::move(labels);
            sinfo_[name] = si;
            scalar_keys_.push_back(name);
        }
    }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lerobot: info.json feature parse error: %s\n", e.what());
        return false;
    }
    std::sort(video_keys_.begin(), video_keys_.end());
    std::sort(scalar_keys_.begin(), scalar_keys_.end());
    if (video_keys_.empty() && scalar_keys_.empty()) {
        std::fprintf(stderr, "lerobot: no video/scalar features\n");
        return false;
    }

    // Episode table. `meta/episodes/**/*.parquet` — one or many files.
    ParquetDB db;
    if (!db.ok()) return false;
    std::string sel = "episode_index, dataset_from_index, dataset_to_index, "
                      "\"data/chunk_index\" AS dci, \"data/file_index\" AS dfi, "
                      "COALESCE(tasks[1], '') AS task";
    for (const auto& k : video_keys_)
        sel += ", " + q("videos/" + k + "/chunk_index") + " AS " + q(k + "|c") + ", " +
               q("videos/" + k + "/file_index") + " AS " + q(k + "|f") + ", " +
               q("videos/" + k + "/from_timestamp") + " AS " + q(k + "|from") + ", " +
               q("videos/" + k + "/to_timestamp") + " AS " + q(k + "|to");
    ParquetDB::Table ep;
    if (!db.query("SELECT " + sel + " FROM read_parquet('" +
                      sql_path(dir_ + "/meta/episodes/*/*.parquet") + "') ORDER BY episode_index",
                  ep))
        return false;

    episodes_.resize(ep.rows);
    auto num = [&](const char* c, size_t r) {
        const auto* col = ep.col(c);
        return col ? col->num[r] : 0.0;
    };
    for (size_t r = 0; r < ep.rows; ++r) {
        Episode& e = episodes_[r];
        e.from_index = (int64_t)num("dataset_from_index", r);
        e.to_index = (int64_t)num("dataset_to_index", r);
        e.data_chunk = (int)num("dci", r);
        e.data_file = (int)num("dfi", r);
        if (const auto* tc = ep.col("task")) e.task = tc->str[r];
        for (const auto& k : video_keys_) {
            e.vid_chunk[k] = (int)num((k + "|c").c_str(), r);
            e.vid_file[k] = (int)num((k + "|f").c_str(), r);
            e.vid_from[k] = num((k + "|from").c_str(), r);
            e.vid_to[k] = num((k + "|to").c_str(), r);
        }
    }
    std::fprintf(stderr, "lerobot: %s, %zu episodes, %zu video, %zu scalar\n", ver.c_str(),
                episodes_.size(), video_keys_.size(), scalar_keys_.size());
    if (episodes_.empty()) return false;

    return select_segment(0);
}

bool LeRobotRecording::select_segment(int i) {
    if (i < 0 || i >= (int)episodes_.size()) return false;
    cur_ep_ = i;
    const Episode& e = episodes_[i];

    const int64_t n_frames = std::max<int64_t>(0, e.to_index - e.from_index);
    seg_len_us_ = fps_ > 0 ? (uint64_t)(n_frames / fps_ * 1e6) : 0;

    // Scalar history: read this episode's rows from the data parquet.
    scalar_hist_.clear();
    if (!scalar_keys_.empty()) {
        ParquetDB db;
        std::string file = data_parquet(dir_, e.data_chunk, e.data_file);
        std::string sel = "timestamp";
        for (const auto& k : scalar_keys_) {
            int d = sinfo_[k].dims;
            for (int c = 0; c < d; ++c)
                sel += ", " + q(k) + "[" + std::to_string(c + 1) + "] AS " + q(k + "|" +
                       std::to_string(c));
        }
        char where[128];
        std::snprintf(where, sizeof(where), " WHERE \"index\" >= %lld AND \"index\" < %lld",
                      (long long)e.from_index, (long long)e.to_index);
        ParquetDB::Table dt;
        if (db.query("SELECT " + sel + " FROM read_parquet('" + sql_path(file) + "')" + where +
                         " ORDER BY \"index\"",
                     dt)) {
            const auto* ts = dt.col("timestamp");
            for (const auto& k : scalar_keys_) {
                int d = sinfo_[k].dims;
                std::vector<const ParquetDB::Column*> cc(d);
                for (int c = 0; c < d; ++c) cc[c] = dt.col(k + "|" + std::to_string(c));
                auto& hist = scalar_hist_[k];
                hist.resize(dt.rows);
                for (size_t r = 0; r < dt.rows; ++r) {
                    hist[r].t_us = ts ? (uint64_t)(ts->num[r] * 1e6) : 0;
                    hist[r].v.resize(d);
                    for (int c = 0; c < d; ++c)
                        hist[r].v[c] = cc[c] ? (float)cc[c]->num[r] : 0.0f;
                }
            }
        }
    }

    // One Mp4Source per camera, restricted to the episode's time slice.
    mp4_.clear();
    for (const auto& k : video_keys_) {
        auto src = std::make_unique<Mp4Source>();
        std::string path = video_mp4(dir_, k, e.vid_chunk.at(k), e.vid_file.at(k));
        uint64_t from_us = (uint64_t)(e.vid_from.at(k) * 1e6);
        uint64_t to_us = (uint64_t)(e.vid_to.at(k) * 1e6);
        if (src->open(path, from_us, to_us)) {
            VideoChannelInfo& vi = vinfo_[k];
            if (src->width() > 0) { vi.width = src->width(); vi.height = src->height(); }
            if (!src->codec().empty()) vi.codec = src->codec();
            vi.frame_count = (uint64_t)n_frames;
            mp4_[k] = std::move(src);
        }
    }
    return true;
}

VideoChannelInfo LeRobotRecording::video_info(const std::string& ch) const {
    auto it = vinfo_.find(ch);
    return it == vinfo_.end() ? VideoChannelInfo{} : it->second;
}

ScalarChannelInfo LeRobotRecording::scalar_info(const std::string& ch) const {
    auto it = sinfo_.find(ch);
    return it == sinfo_.end() ? ScalarChannelInfo{} : it->second;
}

const std::vector<ScalarSample>& LeRobotRecording::scalar_history(const std::string& ch) const {
    auto it = scalar_hist_.find(ch);
    return it == scalar_hist_.end() ? empty_scalar_ : it->second;
}

SegmentInfo LeRobotRecording::segment_info(int i) const {
    SegmentInfo s;
    if (i < 0 || i >= (int)episodes_.size()) return s;
    char nm[32];
    std::snprintf(nm, sizeof(nm), "Episode %d", i);
    s.name = nm;
    const auto& e = episodes_[i];
    int64_t n = std::max<int64_t>(0, e.to_index - e.from_index);
    s.duration_us = fps_ > 0 ? (uint64_t)(n / fps_ * 1e6) : 0;
    return s;
}

bool LeRobotRecording::seek_video(uint64_t target_us, const std::function<bool()>& cancelled,
                                  std::map<std::string, VideoFramePtr>& out) {
    if (cancelled()) return false;

    std::vector<std::pair<std::string, Mp4Source*>> work;
    for (const auto& k : video_keys_) {
        auto it = mp4_.find(k);
        if (it != mp4_.end() && it->second) work.push_back({k, it->second.get()});
    }
    if (work.empty()) return true;

    std::map<std::string, VideoFramePtr> got;
    std::mutex mx;
    auto run = [&](const std::string& k, Mp4Source* s) {
        VideoFramePtr f = s->frame_at(target_us, cancelled);
        std::lock_guard<std::mutex> lk(mx);
        got[k] = std::move(f);
    };

    if (work.size() > 1) {
        std::vector<std::thread> ts;
        for (auto& [k, s] : work) ts.emplace_back(run, k, s);
        for (auto& t : ts) t.join();
    } else {
        run(work[0].first, work[0].second);
    }

    if (cancelled()) return false;
    for (auto& [k, f] : got) out[k] = f; // null => blank
    return true;
}

} // namespace mp
