// SPDX-License-Identifier: Apache-2.0
// SKF media hook — Soup Kitchen Films (patch 0018, 2026-09-02).
//
// C++ replacement for the Soupkitchen python colour_hook + the "switch to a
// fresh SSD copy" half of localise.py. Both used to run in the embedded
// interpreter over the python link (round trips per media, on the same
// link every menu click waits on). Here they run inside the actor system
// on import, for free.
//
//   modify_metadata      per MEDIA SOURCE: colour_pipeline block for the
//                        stock OCIO plugin (show config chosen by the host's
//                        OCIO capability), plus metadata/external/SKF
//                        {show, shot}. Identity = media FILENAME.
//   gather_media_sources if every frame of a /Volumes/ source has a valid
//                        copy (same size, mtime ±tol) in the shared Nuke /
//                        xStudio localise store, add it as a second source
//                        named "SSD". The session actor (patch 0018) makes
//                        an "SSD" source current when XSTUDIO_SKF_PREFER_SSD
//                        is not "0".
//
// Show knowledge is NOT compiled in: apps/xstudio_tools/skf_media_hook.json
// (written by gen_media_hook_config.py from utils/show_paths + show_ocio)
// is read on first use and re-read when its mtime changes.
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

#include "xstudio/media_hook/media_hook.hpp"
#include "xstudio/utility/helpers.hpp"
#include "xstudio/utility/json_store.hpp"
#include "xstudio/utility/string_helpers.hpp"
#include "xstudio/utility/logging.hpp"

namespace fs = std::filesystem;
using namespace xstudio;
using namespace xstudio::media_hook;
using namespace xstudio::utility;

namespace {

struct ShowRule {
    std::string code;
    std::regex shot_re;
    std::string ocio_comp;
    std::string ocio_encode;
    std::string ocio_view;   // review view for scene-linear media -> automatic_view
};

struct Config {
    std::string path;
    time_t mtime = 0;
    std::string base_ocio;
    std::vector<ShowRule> shows;
    std::vector<std::string> movie_exts, still_exts, stores;
    double mtime_tolerance = 2.0;
    bool loaded = false;
};

std::string config_path() {
    if (const char *e = getenv("SKF_MEDIA_HOOK_JSON"); e && *e)
        return e;
    return "/Volumes/soupnas_01/souptracker/DB/skf_pipeline_v01/apps/xstudio_tools/"
           "skf_media_hook.json";
}

bool stat_of(const std::string &p, struct stat *st) { return ::stat(p.c_str(), st) == 0; }

// Static, mutex-guarded config (RodeoFX's lesson: per-show lookups must be
// static or every media source pays the parse).
const Config &config() {
    static Config cfg;
    static std::mutex mx;
    std::lock_guard<std::mutex> l(mx);
    const std::string path = config_path();
    struct stat st;
    const time_t mt = stat_of(path, &st) ? st.st_mtime : 0;
    if (cfg.loaded && cfg.path == path && cfg.mtime == mt)
        return cfg;
    Config fresh;
    fresh.path  = path;
    fresh.mtime = mt;
    try {
        std::ifstream in(path);
        nlohmann::json j;
        in >> j;
        fresh.base_ocio       = j.value("base_ocio", "");
        fresh.mtime_tolerance = j.value("mtime_tolerance", 2.0);
        for (const auto &s : j.value("shows", nlohmann::json::array())) {
            ShowRule r;
            r.code = s.value("code", "");
            r.shot_re =
                std::regex(s.value("shot_regex", "$^"), std::regex::ECMAScript | std::regex::optimize);
            if (s.contains("ocio")) {
                r.ocio_comp   = s["ocio"].value("comp", "");
                r.ocio_encode = s["ocio"].value("encode", "");
                r.ocio_view   = s["ocio"].value("view", "");
            }
            fresh.shows.push_back(std::move(r));
        }
        for (const auto &e : j.value("movie_exts", nlohmann::json::array()))
            fresh.movie_exts.push_back(e.get<std::string>());
        for (const auto &e : j.value("still_exts", nlohmann::json::array()))
            fresh.still_exts.push_back(e.get<std::string>());
        for (const auto &e : j.value("localise_stores", nlohmann::json::array()))
            fresh.stores.push_back(e.get<std::string>());
        fresh.loaded = true;
        spdlog::info("SKF media hook: config {} ({} shows)", path, fresh.shows.size());
    } catch (const std::exception &e) {
        spdlog::warn("SKF media hook: cannot read {}: {}", path, e.what());
        fresh.loaded = true;   // don't retry every media; mtime change re-reads
    }
    cfg = std::move(fresh);
    return cfg;
}

// The store this box uses: first configured store whose directory exists
// (soupripper: /mnt/localise; the fleet: /mnt/caches).
std::string local_store() {
    static std::string store;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const auto &s : config().stores) {
            struct stat st;
            if (stat_of(s, &st) && S_ISDIR(st.st_mode)) {
                store = s;
                break;
            }
        }
        if (store.empty() && !config().stores.empty())
            store = config().stores.back();
        spdlog::info("SKF media hook: localise store {}", store);
    });
    return store;
}

bool under_store(const std::string &path) {
    for (const auto &s : config().stores)
        if (path.rfind(s + "/", 0) == 0)
            return true;
    return false;
}

std::string to_store(const std::string &nas_path) {
    // "/Volumes/H4/x" -> "{store}/_Volumes/H4/x" (Nuke's own layout)
    return local_store() + "/_" + nas_path.substr(1);
}

const ShowRule *show_for(const std::string &basename, std::string *shot) {
    for (const auto &r : config().shows) {
        std::smatch m;
        if (std::regex_search(basename, m, r.shot_re) && m.position(0) == 0) {
            if (shot)
                *shot = m.str(0);
            return &r;
        }
    }
    return nullptr;
}

std::string lower_ext(const std::string &path) {
    std::string e = fs::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

bool has(const std::vector<std::string> &v, const std::string &s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// OCIO config this HOST can load for the show: SKF_OCIO_MAX_PROFILE=2.4
// (ocio24 build) prefers the comp config (ACES 2.0, matches Nuke 17);
// builds without it keep the encode config (ACES 1.3, profile 2.2).
std::string ocio_config_for(const ShowRule *show) {
    double host_max = 2.2;
    if (const char *e = getenv("SKF_OCIO_MAX_PROFILE"); e && *e)
        host_max = atof(e);
    const std::string base = config().base_ocio;
    if (!show)
        return base;
    if (host_max >= 2.4)
        return !show->ocio_comp.empty()     ? show->ocio_comp
               : !show->ocio_encode.empty() ? show->ocio_encode
                                            : base;
    return !show->ocio_encode.empty() ? show->ocio_encode
           : !show->ocio_comp.empty() ? show->ocio_comp
                                      : base;
}

std::string frames_spec(const std::vector<int> &frames_in) {
    std::vector<int> frames = frames_in;
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    std::string out;
    size_t i = 0;
    while (i < frames.size()) {
        size_t j = i;
        while (j + 1 < frames.size() && frames[j + 1] == frames[j] + 1)
            ++j;
        if (!out.empty())
            out += ",";
        out += std::to_string(frames[i]);
        if (j > i)
            out += "-" + std::to_string(frames[j]);
        i = j + 1;
    }
    return out;
}

}  // namespace

class SKFMediaHook : public MediaHook {
  public:
    SKFMediaHook() : MediaHook("SKF") {}
    ~SKFMediaHook() override = default;

    utility::JsonStore modify_metadata(
        const utility::MediaReference &mr, const utility::JsonStore &metadata) override {
        utility::JsonStore result = metadata;
        const caf::uri &uri =
            mr.container() or mr.uris().empty() ? mr.uri() : mr.uris()[0].first;
        const std::string path     = uri_to_posix_path(uri, false);
        const std::string basename = fs::path(path).filename().string();
        std::string shot;
        const ShowRule *show = show_for(basename, &shot);
        const std::string cfg = ocio_config_for(show);
        const std::string ext = lower_ext(path);
        const auto &c         = config();

        nlohmann::json block;
        block["ocio_config"] = cfg;
        block["path"]        = path;
        if (show) {
            block["ocio_context"] = {{"SHOT", shot}, {"SHOW", show->code}};
        }
        if (has(c.movie_exts, ext) || has(c.still_exts, ext)) {
            // display-referred refs/stills: pass straight through
            block["override_view"]    = "Raw";
            block["input_colorspace"] = "Raw:Utility - Raw:raw";
        } else {
            block["working_space"]    = "scene_linear";
            block["input_colorspace"] = "ACEScg:ACES - ACEScg:scene_linear";
            // Config default view can be Raw (H4 ACES 2.0: default display
            // is OCN) -- stamp the show's review view as the automatic view.
            if (show && !show->ocio_view.empty())
                block["automatic_view"] = show->ocio_view;
        }
        result["colour_pipeline"] = block;
        if (show) {
            result["metadata"]["external"]["SKF"]["show"] = show->code;
            result["metadata"]["external"]["SKF"]["shot"] = shot;
        }
        return result;
    }

    std::vector<std::tuple<std::string, caf::uri, xstudio::utility::FrameList>>
    gather_media_sources(
        const std::vector<utility::MediaReference> &refs,
        const std::vector<std::string> &names,
        std::string &preferred_source) override {
        std::vector<std::tuple<std::string, caf::uri, xstudio::utility::FrameList>> out;
        if (has(names, "SSD"))
            return out;   // already attached (session restore, or python localise)
        const std::string store = local_store();
        if (store.empty())
            return out;
        const double tol = config().mtime_tolerance;
        for (const auto &mr : refs) {
            const caf::uri &uri =
                mr.container() or mr.uris().empty() ? mr.uri() : mr.uris()[0].first;
            const std::string path = uri_to_posix_path(uri, false);
            if (path.rfind("/Volumes/", 0) != 0 || under_store(path))
                continue;
            // every concrete file must have a fresh copy
            std::vector<int> frames;
            bool all_fresh = true;
            size_t checked = 0;
            auto fresh = [&](const std::string &nas) {
                struct stat a, b;
                if (!stat_of(nas, &a) || !stat_of(to_store(nas), &b))
                    return false;
                return a.st_size == b.st_size &&
                       std::fabs(double(a.st_mtime) - double(b.st_mtime)) <= tol;
            };
            if (mr.container()) {
                all_fresh = fresh(path);
                checked   = 1;
            } else {
                for (const auto &[furi, frame] : mr.uris()) {
                    ++checked;
                    if (!fresh(uri_to_posix_path(furi, false))) {
                        all_fresh = false;
                        break;
                    }
                    frames.push_back(frame);
                }
            }
            if (!checked || !all_fresh)
                continue;
            const std::string local_pattern = to_store(uri_to_posix_path(mr.uri(), false));
            caf::uri local_uri              = posix_path_to_uri(local_pattern);
            if (mr.container()) {
                out.emplace_back("SSD", local_uri, xstudio::utility::FrameList());
            } else {
                out.emplace_back(
                    "SSD", local_uri, xstudio::utility::FrameList(frames_spec(frames)));
            }
            preferred_source = "SSD";
            spdlog::info(
                "SKF media hook: SSD copy valid for {} ({} files)", fs::path(path).filename().string(), checked);
            break;   // one SSD source per media (the image source)
        }
        return out;
    }
};

extern "C" {
plugin_manager::PluginFactoryCollection *plugin_factory_collection_ptr() {
    return new plugin_manager::PluginFactoryCollection(
        std::vector<std::shared_ptr<plugin_manager::PluginFactory>>(
            {std::make_shared<MediaHookPlugin<MediaHookActor<SKFMediaHook>>>(
                Uuid("7b1c0d2e-5a4f-4c3b-9e8d-0f1a2b3c4d5e"),
                "SKF",
                "Soup Kitchen Films",
                "Per-show OCIO colour + shared SSD localise source",
                semver::version("1.0.0"))}));
}
}
