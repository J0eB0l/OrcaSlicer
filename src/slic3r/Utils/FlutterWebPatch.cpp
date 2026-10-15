#include "FlutterWebPatch.hpp"

#include <cctype>
#include <regex>

namespace Slic3r {

namespace {

// Bump when the rewrite changes, so web views fetch the new output instead of
// an "immutable" copy cached from the previous one.
const char *const k_query_tag = "afc=2";

bool starts_with(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

bool ends_with(const std::string &s, const char *suffix)
{
    const std::string suf(suffix);
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool patch_index(std::string &html)
{
    if (html.find("window.__afcDark") != std::string::npos)
        return false;
    const size_t body = html.find("<body>");
    if (body == std::string::npos)
        return false;
    // __afcPO: the U1 print switches picked on the AFC lane screen, for the print popup.
    html.insert(body + 6, "<script>window.__afcDark=/[?&]dark_mode=1(&|$)/.test(location.search)&&"
                          "/[?&]path=2(&|$)/.test(location.search);window.__afcPO={};"
                          "location.search.replace(/[?&]afc_(bed_level|flow_calibrate|time_lapse_camera)=1(?=&|$)/g,"
                          "function(m,k){window.__afcPO[k]=1;});</script>");
    // The bootstrap script and the preload of main.js.
    static const std::regex hashed_js(R"re(((?:flutter_bootstrap|main)\.[0-9a-f]{16,}\.js)")re");
    html = std::regex_replace(html, hashed_js, std::string("$1?") + k_query_tag + "\"");
    return true;
}

bool patch_bootstrap(std::string &js)
{
    static const std::string key = "\"mainJsPath\":\"";
    const size_t at = js.find(key);
    if (at == std::string::npos)
        return false;
    const size_t start = at + key.size();
    const size_t end   = js.find('"', start);
    if (end == std::string::npos || js.find('?', start) < end)
        return false;
    js.insert(end, std::string("?") + k_query_tag);
    return true;
}

// One of the print popup's two maps of print switches, every value `off`:
//   ["bed_level",<off>,"flow_calibrate",<off>,<name>,<off>]
// where <name> is a variable holding "time_lapse_camera" (or that literal).
struct SwitchMap
{
    size_t      at{std::string::npos};
    size_t      length{0};
    std::string name;
};

SwitchMap find_switch_map(const std::string &js, const std::string &off)
{
    SwitchMap         map;
    const std::string head = "[\"bed_level\"," + off + ",\"flow_calibrate\"," + off + ",";
    const size_t      at   = js.find(head);
    if (at == std::string::npos)
        return map;
    size_t                    p       = at + head.size();
    static const std::string  literal = "\"time_lapse_camera\"";
    if (js.compare(p, literal.size(), literal) == 0) {
        p += literal.size();
    } else {
        while (p < js.size() && (std::isalnum(static_cast<unsigned char>(js[p])) || js[p] == '_' || js[p] == '$'))
            ++p;
        if (p == at + head.size())
            return map;
    }
    const std::string tail = "," + off + "]";
    if (js.compare(p, tail.size(), tail) != 0)
        return map;
    map.name   = js.substr(at + head.size(), p - (at + head.size()));
    map.at     = at;
    map.length = p + tail.size() - at;
    return map;
}

// Snapmaker's print popup (path=4) starts flow calibration, time-lapse and
// auto leveling off, in a map it shows (!1 = false) and one it sends (0/1):
//   A.x(["bed_level",!1,"flow_calibrate",!1,s,!1],...)  A.x(["bed_level",0,"flow_calibrate",0,s,0],...)
// Both start from window.__afcPO instead, the picks from the AFC lane screen.
// Both maps or neither, so the popup never shows one value and sends another.
bool patch_print_switches(std::string &js)
{
    const SwitchMap shown = find_switch_map(js, "!1");
    const SwitchMap sent  = find_switch_map(js, "0");
    if (shown.at == std::string::npos || sent.at == std::string::npos)
        return false;
    auto pick = [](const char *key) { return std::string("(window.__afcPO||{}).") + key; };
    const std::string shown_js = "[\"bed_level\",!!" + pick("bed_level") + ",\"flow_calibrate\",!!" + pick("flow_calibrate") + "," +
                                 shown.name + ",!!" + pick("time_lapse_camera") + "]";
    const std::string sent_js = "[\"bed_level\"," + pick("bed_level") + "?1:0,\"flow_calibrate\"," + pick("flow_calibrate") + "?1:0," +
                                sent.name + "," + pick("time_lapse_camera") + "?1:0]";
    // The later one first, so the earlier offset still holds.
    if (shown.at > sent.at) {
        js.replace(shown.at, shown.length, shown_js);
        js.replace(sent.at, sent.length, sent_js);
    } else {
        js.replace(sent.at, sent.length, sent_js);
        js.replace(shown.at, shown.length, shown_js);
    }
    return true;
}

bool patch_dark_theme(std::string &js)
{
    // The start-up "toggleTheme" sets isSystemTheme and isDark to false, e.g.
    //   q.a=q.b=!1
    //   A.HL("[ThemeVM] toggleTheme, isDark: false, ...")
    // The minified names change with every build; the log text does not.
    static const std::string anchor = "(\"[ThemeVM] toggleTheme";
    const size_t at = js.find(anchor);
    if (at == std::string::npos)
        return false;
    const size_t from = at > 160 ? at - 160 : 0;
    const std::string window = js.substr(from, at + 2 - from);
    static const std::regex assign(R"re(([A-Za-z_$][\w$]*)\.a=\1\.b=!1(\s*[\w$.]+\(")$)re");
    std::smatch m;
    if (!std::regex_search(window, m, assign))
        return false;
    const std::string var = m[1].str();
    const std::string replacement = var + ".a=!1," + var + ".b=!!window.__afcDark" + m[2].str();
    js.replace(from + size_t(m.position(0)), size_t(m.length(0)), replacement);
    return true;
}

bool patch_main(std::string &js)
{
    const bool dark     = patch_dark_theme(js);
    const bool switches = patch_print_switches(js);
    return dark || switches;
}

} // namespace

bool patch_flutter_web_file(const std::string &basename, std::string &content)
{
    if (basename == "index.html")
        return patch_index(content);
    if (starts_with(basename, "flutter_bootstrap.") && ends_with(basename, ".js"))
        return patch_bootstrap(content);
    if (starts_with(basename, "main.") && ends_with(basename, ".js"))
        return patch_main(content);
    return false;
}

} // namespace Slic3r
