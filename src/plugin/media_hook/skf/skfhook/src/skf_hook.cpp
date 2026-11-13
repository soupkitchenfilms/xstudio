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
#include <memory>
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
    // SKF 0028: per-show review settings (utils/show_ocio <- <show>/LUTS/show_ocio.json)
    std::string ocio_review;  // xStudio-only show config, wins over comp/encode
    std::string review_view;  // view when the show config is in use
    std::string exr_input;    // input_colorspace candidates for EXRs
    std::regex view_re;       // group 1 in the media's metadata names its view
    bool has_view_re = false;
};

struct Config {
    std::string path;
    time_t mtime = 0;
    std::string base_ocio;
    std::string studio_ocio;        // SKF 0028: stock studio config for every show without one
    std::string studio_view;        // its view for scene-linear media (stock displays default to Raw)
    std::string default_exr_input;  // EXRs that match no show
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

using ConfigPtr = std::shared_ptr<const Config>;

// Static, mutex-guarded config (RodeoFX's lesson: per-show lookups must be
// static or every media source pays the parse). Callers get an immutable
// SNAPSHOT: a re-read (the JSON's mtime changed) builds a new Config and
// swaps the pointer, so a caller still holding the old one -- and the
// ShowRule pointers into it -- stays valid until it lets go.
ConfigPtr config() {
    static ConfigPtr cfg;
    static time_t bad_mtime = -1;   // a version that failed to parse: don't retry it per media
    static std::mutex mx;
    std::lock_guard<std::mutex> l(mx);
    const std::string path = config_path();
    struct stat st;
    const time_t mt = stat_of(path, &st) ? st.st_mtime : 0;
    if (cfg && cfg->path == path && (cfg->mtime == mt || mt == bad_mtime))
        return cfg;
    auto fresh_ptr = std::make_shared<Config>();
    Config &fresh  = *fresh_ptr;
    fresh.path  = path;
    fresh.mtime = mt;
    try {
        std::ifstream in(path);
        nlohmann::json j;
        in >> j;
        fresh.base_ocio         = j.value("base_ocio", "");
        fresh.studio_ocio       = j.value("studio_ocio", "");
        fresh.studio_view       = j.value("studio_view", "");
        fresh.default_exr_input = j.value("default_exr_input", "");
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
                r.ocio_review = s["ocio"].value("review", "");
                r.review_view = s["ocio"].value("review_view", "");
                r.exr_input   = s["ocio"].value("exr_input", "");
                const std::string vre = s["ocio"].value("view_regex", "");
                if (!vre.empty()) {
                    try {
                        r.view_re     = std::regex(vre, std::regex::ECMAScript);
                        r.has_view_re = true;
                    } catch (const std::regex_error &e) {
                        spdlog::warn("SKF media hook: show {} view_regex '{}': {}", r.code, vre, e.what());
                    }
                }
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
        // e.g. caught mid-rewrite: keep the last good config rather than
        // dropping every show's colour; the next mtime change re-reads.
        if (cfg && cfg->path == path) {
            spdlog::warn("SKF media hook: cannot read {}: {} (keeping previous config)", path, e.what());
            bad_mtime = mt;
            return cfg;
        }
        spdlog::warn("SKF media hook: cannot read {}: {}", path, e.what());
        fresh.loaded = true;   // don't retry every media; mtime change re-reads
    }
    cfg = std::move(fresh_ptr);
    return cfg;
}

// The store this box uses: first configured store whose directory exists
// (soupripper: /mnt/localise; the fleet: /mnt/caches).
std::string local_store() {
    static std::string store;
    static std::once_flag once;
    std::call_once(once, [] {
        const auto c = config();
        for (const auto &s : c->stores) {
            struct stat st;
            if (stat_of(s, &st) && S_ISDIR(st.st_mode)) {
                store = s;
                break;
            }
        }
        if (store.empty() && !c->stores.empty())
            store = c->stores.back();
        spdlog::info("SKF media hook: localise store {}", store);
    });
    return store;
}

bool under_store(const Config &c, const std::string &path) {
    for (const auto &s : c.stores)
        if (path.rfind(s + "/", 0) == 0)
            return true;
    return false;
}

std::string to_store(const std::string &nas_path) {
    // "/Volumes/H4/x" -> "{store}/_Volumes/H4/x" (Nuke's own layout)
    return local_store() + "/_" + nas_path.substr(1);
}

// The returned pointer lives as long as the caller's snapshot `c`.
const ShowRule *show_for(const Config &c, const std::string &basename, std::string *shot) {
    for (const auto &r : c.shows) {
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
std::string ocio_config_for(const Config &c, const ShowRule *show) {
    double host_max = 2.2;
    if (const char *e = getenv("SKF_OCIO_MAX_PROFILE"); e && *e)
        host_max = atof(e);
    const std::string base = !c.studio_ocio.empty() ? c.studio_ocio : c.base_ocio;
    if (!show)
        return base;
    if (!show->ocio_review.empty())
        return show->ocio_review;
    if (host_max >= 2.4)
        return !show->ocio_comp.empty()     ? show->ocio_comp
               : !show->ocio_encode.empty() ? show->ocio_encode
                                            : base;
    return !show->ocio_encode.empty() ? show->ocio_encode
           : !show->ocio_comp.empty() ? show->ocio_comp
                                      : base;
}

// First match of `re` (its group 1) in any string value of the metadata.
// Searched per value, never over metadata.dump(): libstdc++ <regex> recurses
// per character and can overflow the stack on a long input, so values
// longer than kMaxValue (blobs, embedded XML) are skipped.
bool find_in_strings(const nlohmann::json &j, const std::regex &re, std::string *out, int depth = 0) {
    constexpr size_t kMaxValue = 1024;
    if (depth > 16)
        return false;
    if (j.is_string()) {
        const auto &v = j.get_ref<const std::string &>();
        std::smatch m;
        if (v.size() <= kMaxValue && std::regex_search(v, m, re) && m.size() > 1) {
            *out = m.str(1);
            return true;
        }
        return false;
    }
    if (j.is_object() || j.is_array())
        for (const auto &v : j)
            if (find_in_strings(v, re, out, depth + 1))
                return true;
    return false;
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
        const ConfigPtr snap = config();   // one snapshot for the whole call
        const Config &c      = *snap;
        std::string shot;
        const ShowRule *show  = show_for(c, basename, &shot);
        const std::string cfg = ocio_config_for(c, show);
        const std::string ext = lower_ext(path);

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
            block["working_space"] = "scene_linear";
            // SKF 0028: the show's plate space (H4 ACEScg, every other show and
            // unmatched media the studio default ACES2065-1); ACEScg only when
            // the JSON predates both keys.
            const std::string &exr_in = show && !show->exr_input.empty() ? show->exr_input
                                                                         : c.default_exr_input;
            block["input_colorspace"] =
                !exr_in.empty() ? exr_in : std::string("ACEScg:ACES - ACEScg:scene_linear");
            // Config default views can be Raw (H4 ACES 2.0: display OCN; the
            // stock studio config: every display) -- stamp a view.
            std::string view;
            if (cfg == c.studio_ocio && !c.studio_ocio.empty()) {
                view = c.studio_view;
            } else if (show) {
                view = !show->review_view.empty() ? show->review_view : show->ocio_view;
                if (show->has_view_re)
                    find_in_strings(metadata, show->view_re, &view);
            }
            if (!view.empty())
                block["automatic_view"] = view;
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
        const ConfigPtr snap = config();
        const double tol     = snap->mtime_tolerance;
        for (const auto &mr : refs) {
            const caf::uri &uri =
                mr.container() or mr.uris().empty() ? mr.uri() : mr.uris()[0].first;
            const std::string path = uri_to_posix_path(uri, false);
            if (path.rfind("/Volumes/", 0) != 0 || under_store(*snap, path))
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
