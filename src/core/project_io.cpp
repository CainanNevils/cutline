#include "core/project_io.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace cut {

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathStr(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

json toJ(Rational r) { return json::array({r.num, r.den}); }
Rational ratJ(const json& j) {
    Rational r;
    if (j.is_array() && j.size() == 2) {
        r.num = j[0].get<int64_t>();
        r.den = j[1].get<int64_t>();
    }
    return r;
}

json toJ(const Param& p) {
    json j;
    j["v"] = p.value;
    if (!p.keys.empty()) {
        json ks = json::array();
        for (const auto& k : p.keys) ks.push_back(json::array({k.t, k.v, int(k.interp)}));
        j["keys"] = ks;
    }
    return j;
}
Param paramJ(const json& j) {
    Param p;
    p.value = j.value("v", 0.0);
    if (j.contains("keys"))
        for (const auto& k : j["keys"]) p.keys.push_back({k[0].get<Tick>(), k[1].get<double>(), Interp(k[2].get<int>())});
    return p;
}

json toJ(const Effect& e) {
    json j;
    j["type"] = e.type;
    j["enabled"] = e.enabled;
    if (!e.path.empty()) j["path"] = e.path;
    json ps = json::object();
    for (const auto& [k, p] : e.params) ps[k] = toJ(p);
    j["params"] = ps;
    return j;
}
Effect effectJ(const json& j) {
    Effect e;
    e.type = j.value("type", "");
    e.enabled = j.value("enabled", true);
    e.path = j.value("path", "");
    if (j.contains("params"))
        for (auto it = j["params"].begin(); it != j["params"].end(); ++it) e.params[it.key()] = paramJ(it.value());
    return e;
}

json toJ(const TextStyle& t) {
    return json{{"font", t.font},
                {"size", t.size},
                {"weight", t.weight},
                {"italic", t.italic},
                {"fill", t.fill},
                {"stroke", t.stroke},
                {"strokeWidth", t.strokeWidth},
                {"strokeColor", t.strokeColor},
                {"shadow", t.shadow},
                {"shadowX", t.shadowX},
                {"shadowY", t.shadowY},
                {"shadowBlur", t.shadowBlur},
                {"shadowColor", t.shadowColor},
                {"box", t.box},
                {"boxColor", t.boxColor},
                {"boxPadding", t.boxPadding},
                {"boxOpacity", t.boxOpacity},
                {"align", t.align},
                {"lineSpacing", t.lineSpacing},
                {"boxRadius", t.boxRadius},
                {"tracking", t.tracking},
                {"gradient", t.gradient},
                {"fill2", t.fill2}};
}
TextStyle textStyleJ(const json& j) {
    TextStyle t;
    t.font = j.value("font", t.font);
    t.size = j.value("size", t.size);
    t.weight = j.value("weight", t.weight);
    t.italic = j.value("italic", t.italic);
    t.fill = j.value("fill", t.fill);
    t.stroke = j.value("stroke", t.stroke);
    t.strokeWidth = j.value("strokeWidth", t.strokeWidth);
    t.strokeColor = j.value("strokeColor", t.strokeColor);
    t.shadow = j.value("shadow", t.shadow);
    t.shadowX = j.value("shadowX", t.shadowX);
    t.shadowY = j.value("shadowY", t.shadowY);
    t.shadowBlur = j.value("shadowBlur", t.shadowBlur);
    t.shadowColor = j.value("shadowColor", t.shadowColor);
    t.box = j.value("box", t.box);
    t.boxColor = j.value("boxColor", t.boxColor);
    t.boxPadding = j.value("boxPadding", t.boxPadding);
    t.boxOpacity = j.value("boxOpacity", t.boxOpacity);
    t.align = j.value("align", t.align);
    t.lineSpacing = j.value("lineSpacing", t.lineSpacing);
    t.boxRadius = j.value("boxRadius", t.boxRadius);
    t.tracking = j.value("tracking", t.tracking);
    t.gradient = j.value("gradient", t.gradient);
    t.fill2 = j.value("fill2", t.fill2);
    return t;
}

json toJ(const MediaInfo& m) {
    json j{{"valid", m.valid},   {"container", m.container}, {"isImage", m.isImage}, {"hasVideo", m.hasVideo},
           {"duration", m.duration}, {"origin", m.origin}, {"fileSize", m.fileSize}, {"mtime", m.mtime}};
    if (m.hasVideo) {
        const auto& v = m.video;
        j["video"] = json{{"index", v.index},
                          {"width", v.width},
                          {"height", v.height},
                          {"fps", toJ(v.fps)},
                          {"timeBase", toJ(v.timeBase)},
                          {"codec", v.codec},
                          {"pixFmt", v.pixFmt},
                          {"bitDepth", v.bitDepth},
                          {"range", int(v.range)},
                          {"matrix", int(v.matrix)},
                          {"transfer", int(v.transfer)},
                          {"vfr", v.vfr},
                          {"hasAlpha", v.hasAlpha},
                          {"duration", v.duration},
                          {"startTime", v.startTime}};
    }
    json as = json::array();
    for (const auto& a : m.audio)
        as.push_back(json{{"index", a.index},
                          {"sampleRate", a.sampleRate},
                          {"channels", a.channels},
                          {"codec", a.codec},
                          {"title", a.title},
                          {"language", a.language},
                          {"duration", a.duration},
                          {"startTime", a.startTime}});
    j["audio"] = as;
    return j;
}
MediaInfo mediaInfoJ(const json& j) {
    MediaInfo m;
    m.valid = j.value("valid", false);
    m.container = j.value("container", "");
    m.isImage = j.value("isImage", false);
    m.hasVideo = j.value("hasVideo", false);
    m.duration = j.value("duration", Tick(0));
    m.origin = j.value("origin", Tick(0));
    m.fileSize = j.value("fileSize", int64_t(0));
    m.mtime = j.value("mtime", int64_t(0));
    if (j.contains("video")) {
        const json& v = j["video"];
        auto& o = m.video;
        o.index = v.value("index", -1);
        o.width = v.value("width", 0);
        o.height = v.value("height", 0);
        o.fps = ratJ(v["fps"]);
        o.timeBase = ratJ(v["timeBase"]);
        o.codec = v.value("codec", "");
        o.pixFmt = v.value("pixFmt", "");
        o.bitDepth = v.value("bitDepth", 8);
        o.range = ColorRange(v.value("range", 0));
        o.matrix = ColorMatrix(v.value("matrix", 0));
        o.transfer = ColorTransfer(v.value("transfer", 0));
        o.vfr = v.value("vfr", false);
        o.hasAlpha = v.value("hasAlpha", false);
        o.duration = v.value("duration", Tick(0));
        o.startTime = v.value("startTime", Tick(0));
    }
    if (j.contains("audio"))
        for (const auto& a : j["audio"]) {
            AudioStreamInfo o;
            o.index = a.value("index", -1);
            o.sampleRate = a.value("sampleRate", 0);
            o.channels = a.value("channels", 0);
            o.codec = a.value("codec", "");
            o.title = a.value("title", "");
            o.language = a.value("language", "");
            o.duration = a.value("duration", Tick(0));
            o.startTime = a.value("startTime", Tick(0));
            m.audio.push_back(o);
        }
    return m;
}

json toJ(const Clip& c) {
    json j{{"id", c.id},
           {"track", c.trackId},
           {"kind", int(c.kind)},
           {"asset", c.assetId},
           {"stream", c.stream},
           {"name", c.name},
           {"start", c.start},
           {"duration", c.duration},
           {"sourceIn", c.sourceIn},
           {"speed", c.speed},
           {"reverse", c.reverse},
           {"frameBlend", c.frameBlend},
           {"enabled", c.enabled},
           {"link", c.linkId},
           {"fit", int(c.fit)},
           {"gainDb", c.gainDb},
           {"fadeIn", c.fadeIn},
           {"fadeOut", c.fadeOut}};
    if (c.label) j["label"] = c.label;
    if (c.kind == ClipKind::Nested) j["nested"] = c.nestedSeq;
    if (c.kind == ClipKind::Matte) j["matte"] = c.matteColor;
    if (!c.motion.type.empty()) j["motion"] = toJ(c.motion);
    if (!c.opacity.type.empty()) j["opacity"] = toJ(c.opacity);
    if (!c.volume.type.empty()) j["volume"] = toJ(c.volume);
    json es = json::array();
    for (const auto& e : c.effects) es.push_back(toJ(e));
    j["effects"] = es;
    if (c.kind == ClipKind::Text) {
        j["text"] = c.text;
        j["textStyle"] = toJ(c.textStyle);
    }
    return j;
}
Clip clipJ(const json& j) {
    Clip c;
    c.id = j.value("id", Id(0));
    c.trackId = j.value("track", Id(0));
    c.kind = ClipKind(j.value("kind", 0));
    c.assetId = j.value("asset", Id(0));
    c.stream = j.value("stream", 0);
    c.name = j.value("name", "");
    c.start = j.value("start", Tick(0));
    c.duration = j.value("duration", Tick(0));
    c.sourceIn = j.value("sourceIn", Tick(0));
    c.speed = j.value("speed", 1.0);
    c.reverse = j.value("reverse", false);
    c.frameBlend = j.value("frameBlend", false);
    c.enabled = j.value("enabled", true);
    c.linkId = j.value("link", Id(0));
    c.fit = FitMode(j.value("fit", 0));
    c.gainDb = j.value("gainDb", 0.0);
    c.fadeIn = j.value("fadeIn", Tick(0));
    c.fadeOut = j.value("fadeOut", Tick(0));
    c.label = j.value("label", uint32_t(0));
    c.nestedSeq = j.value("nested", Id(0));
    c.matteColor = j.value("matte", uint32_t(0xFF1E1E1E));
    if (j.contains("motion")) c.motion = effectJ(j["motion"]);
    if (j.contains("opacity")) c.opacity = effectJ(j["opacity"]);
    if (j.contains("volume")) c.volume = effectJ(j["volume"]);
    if (j.contains("effects"))
        for (const auto& e : j["effects"]) c.effects.push_back(effectJ(e));
    c.text = j.value("text", "");
    if (j.contains("textStyle")) c.textStyle = textStyleJ(j["textStyle"]);
    return c;
}

json toJ(const Sequence& s) {
    json j;
    j["name"] = s.name;
    j["uid"] = s.uid;
    j["width"] = s.settings.width;
    j["height"] = s.settings.height;
    j["fps"] = toJ(s.settings.fps);
    j["sampleRate"] = s.settings.sampleRate;
    j["nextId"] = s.nextId;
    j["in"] = s.inPoint;
    j["out"] = s.outPoint;
    j["masterVolumeDb"] = s.masterVolumeDb;
    json ts = json::array();
    for (const auto& t : s.tracks) {
        json tj{{"id", t.id},
                          {"kind", int(t.kind)},
                          {"name", t.name},
                          {"locked", t.locked},
                          {"muted", t.muted},
                          {"solo", t.solo},
                          {"visible", t.visible},
                          {"height", t.height},
                          {"volumeDb", t.volumeDb},
                          {"pan", t.pan}};
        if (t.duckUnder) {
            tj["duckUnder"] = t.duckUnder;
            tj["duckDb"] = t.duckDb;
        }
        ts.push_back(tj);
    }
    j["tracks"] = ts;
    json cs = json::array();
    for (const auto& c : s.clips) cs.push_back(toJ(c));
    j["clips"] = cs;
    json tr = json::array();
    for (const auto& t : s.transitions)
        tr.push_back(json{{"id", t.id},
                          {"track", t.trackId},
                          {"a", t.clipA},
                          {"b", t.clipB},
                          {"type", int(t.type)},
                          {"duration", t.duration},
                          {"align", int(t.align)}});
    j["transitions"] = tr;
    json ms = json::array();
    for (const auto& m : s.markers) ms.push_back(json{{"id", m.id}, {"t", m.t}, {"name", m.name}, {"color", m.color}});
    j["markers"] = ms;
    json caps = json::array();
    for (const auto& c : s.captions)
        caps.push_back(json{{"id", c.id}, {"start", c.start}, {"end", c.end}, {"text", c.text}});
    j["captions"] = caps;
    j["captionStyle"] = json{{"text", toJ(s.captionStyle.text)}, {"marginBottom", s.captionStyle.marginBottom}};
    return j;
}
Sequence sequenceJ(const json& j) {
    Sequence s;
    s.name = j.value("name", s.name);
    s.uid = j.value("uid", Id(0));
    s.settings.width = j.value("width", 1920);
    s.settings.height = j.value("height", 1080);
    s.settings.fps = ratJ(j["fps"]);
    if (!s.settings.fps.valid()) s.settings.fps = {30, 1};
    s.settings.sampleRate = j.value("sampleRate", 48000);
    s.nextId = j.value("nextId", Id(1));
    s.inPoint = j.value("in", kInvalidTick);
    s.outPoint = j.value("out", kInvalidTick);
    s.masterVolumeDb = j.value("masterVolumeDb", 0.0);
    for (const auto& t : j["tracks"]) {
        Track o;
        o.id = t.value("id", Id(0));
        o.kind = TrackKind(t.value("kind", 0));
        o.name = t.value("name", "");
        o.locked = t.value("locked", false);
        o.muted = t.value("muted", false);
        o.solo = t.value("solo", false);
        o.visible = t.value("visible", true);
        o.height = t.value("height", 56);
        o.volumeDb = t.value("volumeDb", 0.0);
        o.pan = t.value("pan", 0.0);
        o.duckUnder = t.value("duckUnder", Id(0));
        o.duckDb = t.value("duckDb", -12.0);
        s.tracks.push_back(o);
    }
    for (const auto& c : j["clips"]) s.clips.push_back(clipJ(c));
    for (const auto& t : j["transitions"]) {
        Transition o;
        o.id = t.value("id", Id(0));
        o.trackId = t.value("track", Id(0));
        o.clipA = t.value("a", Id(0));
        o.clipB = t.value("b", Id(0));
        o.type = TransitionType(t.value("type", 0));
        o.duration = t.value("duration", kTicksPerSecond);
        o.align = TransitionAlign(t.value("align", 0));
        s.transitions.push_back(o);
    }
    for (const auto& m : j["markers"]) {
        Marker o;
        o.id = m.value("id", Id(0));
        o.t = m.value("t", Tick(0));
        o.name = m.value("name", "");
        o.color = m.value("color", 0xFF3FB950u);
        s.markers.push_back(o);
    }
    for (const auto& c : j["captions"]) {
        Caption o;
        o.id = c.value("id", Id(0));
        o.start = c.value("start", Tick(0));
        o.end = c.value("end", Tick(0));
        o.text = c.value("text", "");
        s.captions.push_back(o);
    }
    if (j.contains("captionStyle")) {
        s.captionStyle.text = textStyleJ(j["captionStyle"]["text"]);
        s.captionStyle.marginBottom = j["captionStyle"].value("marginBottom", 8.0);
    }
    return s;
}

} // namespace

std::string textStyleToJson(const TextStyle& s) { return toJ(s).dump(); }

bool textStyleFromJson(const std::string& text, TextStyle& out) {
    try {
        out = textStyleJ(json::parse(text));
        return true;
    } catch (...) {
        return false;
    }
}

std::string effectToJson(const Effect& e) { return toJ(e).dump(); }

bool effectFromJson(const std::string& text, Effect& out) {
    try {
        out = effectJ(json::parse(text));
        return !out.type.empty() && findEffectDef(out.type) != nullptr;
    } catch (...) {
        return false;
    }
}

bool migrateProjectJson(void* obj, int fromVersion, std::string& error) {
    json& j = *static_cast<json*>(obj);
    if (fromVersion > Project::kSchemaVersion) {
        error = "This project was saved by a newer version of Cutline (schema " + std::to_string(fromVersion) + ").";
        return false;
    }
    // v1 is the first schema; future upgrades go here (e.g. if (fromVersion < 2) { ... }).
    j["schemaVersion"] = Project::kSchemaVersion;
    return true;
}

std::string serializeProject(const Project& p, const std::string& projectPath) {
    json j;
    j["format"] = "cutline-project";
    j["schemaVersion"] = Project::kSchemaVersion;
    j["name"] = p.name;
    j["settings"] = json{{"cacheDir", p.settings.cacheDir}};
    j["nextAssetId"] = p.nextAssetId;
    j["nextSequenceUid"] = p.nextSequenceUid;
    json bins = json::array();
    for (const auto& b : p.bins) bins.push_back(json{{"id", b.id}, {"parent", b.parent}, {"name", b.name}});
    j["bins"] = bins;
    fs::path projDir = projectPath.empty() ? fs::path() : u8path(projectPath).parent_path();
    json assets = json::array();
    for (const auto& a : p.assets) {
        std::string rel = a.relPath;
        if (!projDir.empty() && !a.path.empty()) {
            std::error_code ec;
            fs::path r = fs::relative(u8path(a.path), projDir, ec);
            if (!ec && !r.empty()) {
                auto g = r.generic_u8string();
                rel = std::string(g.begin(), g.end());
            }
        }
        assets.push_back(json{{"id", a.id},
                              {"bin", a.binId},
                              {"name", a.name},
                              {"path", a.path},
                              {"relPath", rel},
                              {"info", toJ(a.info)}});
    }
    j["assets"] = assets;
    j["sequence"] = toJ(p.sequence);
    if (!p.otherSequences.empty()) {
        json os = json::array();
        for (const auto& s : p.otherSequences) os.push_back(toJ(s));
        j["otherSequences"] = os;
        j["activeSequence"] = p.activeSequence;
    }
    return j.dump(1, '\t') + "\n";
}

bool deserializeProject(const std::string& text, Project& out, std::string& error) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("Not a valid project file: ") + e.what();
        return false;
    }
    try {
        if (j.value("format", "") != "cutline-project") {
            error = "Not a Cutline project file.";
            return false;
        }
        int ver = j.value("schemaVersion", 0);
        if (ver != Project::kSchemaVersion && !migrateProjectJson(&j, ver, error)) return false;
        Project p;
        p.name = j.value("name", "Untitled");
        p.settings.cacheDir = j["settings"].value("cacheDir", "");
        p.nextAssetId = j.value("nextAssetId", Id(1));
        for (const auto& b : j["bins"]) p.bins.push_back({b.value("id", Id(0)), b.value("parent", Id(0)), b.value("name", "")});
        for (const auto& a : j["assets"]) {
            Asset o;
            o.id = a.value("id", Id(0));
            o.binId = a.value("bin", Id(0));
            o.name = a.value("name", "");
            o.path = a.value("path", "");
            o.relPath = a.value("relPath", "");
            o.info = mediaInfoJ(a["info"]);
            p.assets.push_back(o);
        }
        p.sequence = sequenceJ(j["sequence"]);
        if (j.contains("otherSequences"))
            for (const auto& s : j["otherSequences"]) p.otherSequences.push_back(sequenceJ(s));
        p.activeSequence = std::clamp(j.value("activeSequence", 0), 0, int(p.otherSequences.size()));
        // sequence identities (projects from before nesting have none): unique, counter past them all
        p.nextSequenceUid = std::max<Id>(j.value("nextSequenceUid", Id(2)), 2);
        std::set<Id> seen;
        auto fix = [&](Sequence& s) {
            if (s.uid == 0 || seen.count(s.uid)) s.uid = seen.empty() && s.uid == 0 ? 1 : 0;
            if (s.uid == 0 || seen.count(s.uid)) s.uid = p.nextSequenceUid++;
            seen.insert(s.uid);
            p.nextSequenceUid = std::max(p.nextSequenceUid, s.uid + 1);
        };
        fix(p.sequence);
        for (auto& s : p.otherSequences) fix(s);
        out = std::move(p);
        return true;
    } catch (const std::exception& e) {
        error = std::string("Project file is damaged: ") + e.what();
        return false;
    }
}

bool readFileUtf8(const std::string& path, std::string& out) {
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    fs::path p = u8path(path);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), std::streamsize(data.size()));
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool fileStat(const std::string& path, int64_t& size, int64_t& mtime) {
    std::error_code ec;
    fs::path p = u8path(path);
    auto sz = fs::file_size(p, ec);
    if (ec) return false;
    auto t = fs::last_write_time(p, ec);
    if (ec) return false;
    size = int64_t(sz);
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    mtime = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    return true;
}

bool saveProjectFile(const Project& p, const std::string& path, std::string& error) {
    if (!writeFileAtomic(path, serializeProject(p, path))) {
        error = "Could not write " + path + " (disk full or no permission?)";
        return false;
    }
    return true;
}

bool loadProjectFile(const std::string& path, Project& out, std::string& error) {
    std::string text;
    if (!readFileUtf8(path, text)) {
        error = "Could not read " + path;
        return false;
    }
    return deserializeProject(text, out, error);
}

int resolveMediaPaths(Project& p, const std::string& projectPath) {
    int offline = 0;
    fs::path projDir = projectPath.empty() ? fs::path() : u8path(projectPath).parent_path();
    for (auto& a : p.assets) {
        int64_t size = 0, mtime = 0;
        bool ok = fileStat(a.path, size, mtime);
        if (!ok && !a.relPath.empty() && !projDir.empty()) {
            fs::path cand = (projDir / u8path(a.relPath)).lexically_normal();
            if (fileStat(pathStr(cand), size, mtime)) {
                a.path = pathStr(cand.make_preferred());
                ok = true;
            }
        }
        a.offline = !ok || (a.info.fileSize && size != a.info.fileSize);
        if (a.offline) ++offline;
    }
    return offline;
}

} // namespace cut
