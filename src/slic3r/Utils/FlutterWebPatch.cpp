#include "FlutterWebPatch.hpp"

#include <regex>

namespace Slic3r {

namespace {

// Bump when the rewrite changes, so web views fetch the new output instead of
// an "immutable" copy cached from the previous one.
const char *const k_query_tag = "afc=1";

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
    html.insert(body + 6, "<script>window.__afcDark=/[?&]dark_mode=1(&|$)/.test(location.search)&&"
                          "/[?&]path=2(&|$)/.test(location.search);</script>");
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

bool patch_main(std::string &js)
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
