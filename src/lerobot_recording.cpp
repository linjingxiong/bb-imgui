#include "lerobot_recording.h"

#include "parquet.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace mp {

namespace fs = std::filesystem;

namespace {
std::string video_mp4(const std::string& dir, const std::string& key, int chunk, int file) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s/videos/%s/chunk-%03d/file-%03d.mp4", dir.c_str(),
                  key.c_str(), chunk, file);
    return buf;
}
// SQL-quote a column name that contains '/' or '.'
std::string q(const std::string& name) { return "\"" + name + "\""; }

// LeRobot `names` can be a flat list, a {group: [names]} dict, or a list of
// lists — flatten to the leaf strings in order.
void flatten_names(const nlohmann::json& j, std::vector<std::string>& out) {
    if (j.is_string()) {
        out.push_back(j.get<std::string>());
    } else if (j.is_array()) {
        for (const auto& e : j) flatten_names(e, out);
    } else if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) flatten_names(it.value(), out);
    }
}
} // namespace

LeRobotRecording::~LeRobotRecording() {
    mp4_.clear();
    img_.clear();
}

// Learn which file holds each episode's rows — some datasets have a broken
// data/file_index or 1-based file names, so the mapping can't be trusted by
// name. parquet_metadata() reads only the footer, so this stays cheap even for
// many large files.
void LeRobotRecording::build_episode_file_map() {
    ep_ranges_.clear();
    std::string glob = dir_;
    for (auto& c : glob) if (c == '\\') c = '/';
    glob += "/data/**/*.parquet";
    ParquetDB db;
    ParquetDB::Table t;
    if (db.query("SELECT file_name AS fn, min(stats_min_value::BIGINT) AS lo, "
                 "max(stats_max_value::BIGINT) AS hi FROM parquet_metadata('" +
                     sql_path(glob) + "') WHERE path_in_schema = 'episode_index' GROUP BY file_name",
                 t)) {
        const auto* fn = t.col("fn");
        const auto* lo = t.col("lo");
        const auto* hi = t.col("hi");
        if (fn && lo && hi)
            for (size_t r = 0; r < t.rows; ++r)
                ep_ranges_.push_back({fn->str[r], (int)lo->num[r], (int)hi->num[r]});
    }
}

std::string LeRobotRecording::data_file_path(const Episode& e) const {
    for (const auto& r : ep_ranges_)
        if (e.ep_index >= r.lo && e.ep_index <= r.hi && fs::exists(r.path)) return r.path;
    // Fallback: templated name, then the i-th sorted file.
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s/data/chunk-%03d/file-%03d.parquet", dir_.c_str(),
                  e.data_chunk, e.data_file);
    if (fs::exists(buf)) return buf;
    std::vector<std::string> all;
    std::error_code ec;
    for (auto& d : fs::recursive_directory_iterator(dir_ + "/data", ec))
        if (!ec && d.path().extension() == ".parquet") all.push_back(d.path().string());
    std::sort(all.begin(), all.end());
    for (int i = 0; i < (int)episodes_.size(); ++i)
        if (&episodes_[i] == &e && i < (int)all.size()) return all[i];
    return buf;
}

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
        if (dtype == "video" || dtype == "image") {
            VideoChannelInfo vi;
            vi.valid = true;
            vi.display_name = name;
            // shape is [H,W,C] or [C,H,W] — a hint only; real dims come from
            // the first decoded frame. Pick the two non-3 entries as H,W.
            if (shape.size() == 3) {
                int a = shape[0], b = shape[1], c = shape[2];
                if (a == 3) { vi.height = b; vi.width = c; }
                else { vi.height = a; vi.width = b; }
            }
            // the codec block is "video_info" in some datasets, "info" in others
            nlohmann::json vinf =
                feat.value("video_info", feat.value("info", nlohmann::json::object()));
            vi.codec = dtype == "image" ? "png" : vinf.value("video.codec", "");
            vi.fps = vinf.value("video.fps", fps_);
            vinfo_[name] = vi;
            video_keys_.push_back(name);
            vsrc_kind_[name] = dtype == "image" ? VSrc::Image : VSrc::Mp4;
        } else {
            bool is_index = false;
            for (auto* c : kIndexCols) if (name == c) is_index = true;
            if (is_index || !feat.contains("names") || feat["names"].is_null()) continue;
            int dims = shape.empty() ? 1 : shape[0];
            if (dims < 1) continue;
            std::vector<std::string> labels;
            flatten_names(feat["names"], labels);
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
    const std::string ep_glob = sql_path(dir_ + "/meta/episodes/*/*.parquet");

    // Does the table carry per-camera mp4 slice columns? (Absent for
    // image-mode datasets.)
    {
        ParquetDB::Table probe;
        if (db.query("SELECT * FROM read_parquet('" + ep_glob + "') LIMIT 0", probe) &&
            !video_keys_.empty())
            episodes_have_video_slices_ =
                probe.col("videos/" + video_keys_[0] + "/from_timestamp") != nullptr;
    }

    std::string sel = "episode_index, dataset_from_index, dataset_to_index, "
                      "COALESCE(length, 0) AS len, "
                      "COALESCE(\"data/chunk_index\", 0) AS dci, "
                      "COALESCE(\"data/file_index\", 0) AS dfi, "
                      "COALESCE(tasks[1], '') AS task";
    if (episodes_have_video_slices_)
        for (const auto& k : video_keys_)
            sel += ", " + q("videos/" + k + "/chunk_index") + " AS " + q(k + "|c") + ", " +
                   q("videos/" + k + "/file_index") + " AS " + q(k + "|f") + ", " +
                   q("videos/" + k + "/from_timestamp") + " AS " + q(k + "|from") + ", " +
                   q("videos/" + k + "/to_timestamp") + " AS " + q(k + "|to");
    ParquetDB::Table ep;
    if (!db.query("SELECT " + sel + " FROM read_parquet('" + ep_glob + "') ORDER BY episode_index",
                  ep))
        return false;

    episodes_.resize(ep.rows);
    auto num = [&](const char* c, size_t r) {
        const auto* col = ep.col(c);
        return col ? col->num[r] : 0.0;
    };
    for (size_t r = 0; r < ep.rows; ++r) {
        Episode& e = episodes_[r];
        e.ep_index = (int)num("episode_index", r);
        e.length = (int)num("len", r);
        e.from_index = (int64_t)num("dataset_from_index", r);
        e.to_index = (int64_t)num("dataset_to_index", r);
        e.data_chunk = (int)num("dci", r);
        e.data_file = (int)num("dfi", r);
        if (const auto* tc = ep.col("task")) e.task = tc->str[r];
        if (episodes_have_video_slices_)
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

    build_episode_file_map();
    return select_segment(0);
}

bool LeRobotRecording::select_segment(int i) {
    if (i < 0 || i >= (int)episodes_.size()) return false;
    cur_ep_ = i;
    const Episode& e = episodes_[i];

    // Row selection keys off episode_index / frame_index: some published
    // datasets ship a broken dataset_from_index/dataset_to_index (e.g. every
    // episode clamped to the same range), but episode_index in the data
    // parquet is always right.
    int64_t n_frames = e.length > 0 ? e.length : std::max<int64_t>(0, e.to_index - e.from_index);
    seg_len_us_ = fps_ > 0 ? (uint64_t)(n_frames / fps_ * 1e6) : 0;

    const std::string file = data_file_path(e);

    // Scalar history: read this episode's rows from the data parquet.
    scalar_hist_.clear();
    if (!scalar_keys_.empty() && scalar_db_.ok()) {
        ParquetDB& db = scalar_db_;
        std::string sel = "timestamp";
        for (const auto& k : scalar_keys_) {
            int d = sinfo_[k].dims;
            for (int c = 0; c < d; ++c)
                sel += ", " + q(k) + "[" + std::to_string(c + 1) + "] AS " + q(k + "|" +
                       std::to_string(c));
        }
        char where[96];
        std::snprintf(where, sizeof(where), " WHERE \"episode_index\" = %d", e.ep_index);
        ParquetDB::Table dt;
        if (db.query("SELECT " + sel + " FROM read_parquet('" + sql_path(file) + "')" + where +
                         " ORDER BY \"frame_index\"",
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

    // Image columns: reuse the existing sources across a switch (keeps their
    // DuckDB connections + decoders), and (re)open all cameras in parallel.
    std::vector<std::string> img_keys, mp4_keys;
    for (const auto& k : video_keys_)
        (vsrc_kind_[k] == VSrc::Image ? img_keys : mp4_keys).push_back(k);

    for (const auto& k : img_keys) {
        vinfo_[k].frame_count = (uint64_t)n_frames;
        if (!img_.count(k)) img_[k] = std::make_unique<ImageColumnSource>();
    }
    // drop sources for cameras that no longer exist
    for (auto it = img_.begin(); it != img_.end();)
        it = std::find(img_keys.begin(), img_keys.end(), it->first) == img_keys.end()
                 ? img_.erase(it)
                 : std::next(it);

    {
        std::vector<std::thread> ts;
        const int ep = e.ep_index;
        for (const auto& k : img_keys) {
            ImageColumnSource* s = img_[k].get();
            const int w = vinfo_[k].width, h = vinfo_[k].height;
            ts.emplace_back([s, file, k, ep, w, h] { s->reopen(file, k, ep, w, h); });
        }
        for (auto& t : ts) t.join();
    }
    for (const auto& k : img_keys)
        if (img_[k]->width() > 0) {
            vinfo_[k].width = img_[k]->width();
            vinfo_[k].height = img_[k]->height();
        }

    // mp4 cameras: still recreated (cheap — just avformat_open_input).
    mp4_.clear();
    for (const auto& k : mp4_keys) {
        VideoChannelInfo& vi = vinfo_[k];
        vi.frame_count = (uint64_t)n_frames;
        auto src = std::make_unique<Mp4Source>();
        std::string path = video_mp4(dir_, k, e.vid_chunk.count(k) ? e.vid_chunk.at(k) : 0,
                                     e.vid_file.count(k) ? e.vid_file.at(k) : 0);
        uint64_t from_us = (uint64_t)((e.vid_from.count(k) ? e.vid_from.at(k) : 0.0) * 1e6);
        uint64_t to_us = (uint64_t)((e.vid_to.count(k) ? e.vid_to.at(k) : 0.0) * 1e6);
        if (src->open(path, from_us, to_us)) {
            if (src->width() > 0) { vi.width = src->width(); vi.height = src->height(); }
            if (!src->codec().empty()) vi.codec = src->codec();
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
    s.task = e.task;
    int64_t n = e.length > 0 ? e.length : std::max<int64_t>(0, e.to_index - e.from_index);
    s.duration_us = fps_ > 0 ? (uint64_t)(n / fps_ * 1e6) : 0;
    return s;
}

bool LeRobotRecording::seek_video(uint64_t target_us, const std::function<bool()>& cancelled,
                                  std::map<std::string, VideoFramePtr>& out) {
    if (cancelled()) return false;

    // Per camera, a closure that produces the frame (mp4 or image column).
    std::vector<std::pair<std::string, std::function<VideoFramePtr()>>> work;
    for (const auto& k : video_keys_) {
        if (auto it = mp4_.find(k); it != mp4_.end() && it->second) {
            Mp4Source* s = it->second.get();
            work.push_back({k, [=] { return s->frame_at(target_us, cancelled); }});
        } else if (auto it2 = img_.find(k); it2 != img_.end() && it2->second) {
            ImageColumnSource* s = it2->second.get();
            work.push_back({k, [=] { return s->frame_at(target_us, cancelled); }});
        }
    }
    if (work.empty()) return true;

    std::map<std::string, VideoFramePtr> got;
    std::mutex mx;
    auto run = [&](const std::string& k, const std::function<VideoFramePtr()>& fn) {
        VideoFramePtr f = fn();
        std::lock_guard<std::mutex> lk(mx);
        got[k] = std::move(f);
    };

    if (work.size() > 1) {
        std::vector<std::thread> ts;
        for (auto& [k, fn] : work) ts.emplace_back(run, k, fn);
        for (auto& t : ts) t.join();
    } else {
        run(work[0].first, work[0].second);
    }

    if (cancelled()) return false;
    // A null frame here means "not decoded yet" (loader still catching up after
    // a switch), not "blank" — leave the panel showing whatever it had rather
    // than flashing empty.
    for (auto& [k, f] : got)
        if (f) out[k] = f;
    return true;
}

} // namespace mp
