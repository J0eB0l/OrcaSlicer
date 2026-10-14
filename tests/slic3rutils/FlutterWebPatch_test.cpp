#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/FlutterWebPatch.hpp"

#include <fstream>
#include <sstream>

using namespace Slic3r;

namespace {

std::string read_bundled(const std::string &name)
{
    std::ifstream f(std::string(FLUTTER_WEB_DIR) + "/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("index.html sets the dark flag and tags the scripts", "[FlutterWebPatch]")
{
    std::string html = R"(<html><head><link rel="preload" href="main.0123456789abcdef.js" as="script"></head><body> )"
                       R"(<script src="flutter_overlay.0123456789abcdef.js"></script> )"
                       R"(<script src="flutter_bootstrap.fedcba9876543210.js" async></script></body></html>)";
    REQUIRE(patch_flutter_web_file("index.html", html));
    CHECK(html.find("<body><script>window.__afcDark=") != std::string::npos);
    CHECK(html.find("path=2") != std::string::npos);
    CHECK(html.find(R"(href="main.0123456789abcdef.js?afc=1")") != std::string::npos);
    CHECK(html.find(R"(src="flutter_bootstrap.fedcba9876543210.js?afc=1")") != std::string::npos);
    CHECK(html.find(R"(src="flutter_overlay.0123456789abcdef.js")") != std::string::npos);

    const std::string once = html;
    CHECK_FALSE(patch_flutter_web_file("index.html", html));
    CHECK(html == once);
}

TEST_CASE("bootstrap loads the tagged main script", "[FlutterWebPatch]")
{
    std::string js = R"(_flutter.buildConfig={"builds":[{"compileTarget":"dart2js","renderer":"canvaskit","mainJsPath":"main.0123456789abcdef.js"},{}]};)";
    REQUIRE(patch_flutter_web_file("flutter_bootstrap.fedcba9876543210.js", js));
    CHECK(js.find(R"("mainJsPath":"main.0123456789abcdef.js?afc=1")") != std::string::npos);
    CHECK_FALSE(patch_flutter_web_file("flutter_bootstrap.fedcba9876543210.js", js));
}

TEST_CASE("main script reads the dark flag at start-up", "[FlutterWebPatch]")
{
    std::string js = "for(;;)switch(s){case 0:q.a=q.b=!1\nA.HL(\"[ThemeVM] toggleTheme, isDark: false, isSystemTheme: false\")\nif(q.a)q.d=1";
    REQUIRE(patch_flutter_web_file("main.0123456789abcdef.js", js));
    CHECK(js.find("case 0:q.a=!1,q.b=!!window.__afcDark\nA.HL(\"[ThemeVM] toggleTheme") != std::string::npos);
    CHECK_FALSE(patch_flutter_web_file("main.0123456789abcdef.js", js));

    SECTION("other minified names") {
        std::string other = "case 0:$x.a=$x.b=!1\nB.Zq(\"[ThemeVM] toggleTheme, isDark: false\")";
        REQUIRE(patch_flutter_web_file("main.fedcba9876543210.js", other));
        CHECK(other == "case 0:$x.a=!1,$x.b=!!window.__afcDark\nB.Zq(\"[ThemeVM] toggleTheme, isDark: false\")");
    }
    SECTION("a changed page is left alone") {
        std::string changed = "case 0:q.a=q.b=p\nA.HL(\"[ThemeVM] toggleTheme, isDark: false\")";
        const std::string before = changed;
        CHECK_FALSE(patch_flutter_web_file("main.0123456789abcdef.js", changed));
        CHECK(changed == before);
    }
}

TEST_CASE("other files are served as they are", "[FlutterWebPatch]")
{
    std::string js = "q.a=q.b=!1\nA.HL(\"[ThemeVM] toggleTheme\")";
    CHECK_FALSE(patch_flutter_web_file("flutter_overlay.0123456789abcdef.js", js));
    std::string html = "<html><body></body></html>";
    CHECK_FALSE(patch_flutter_web_file("splash.html", html));
}

TEST_CASE("the bundled Snapmaker page can be patched", "[FlutterWebPatch]")
{
    std::string html = read_bundled("index.html");
    REQUIRE_FALSE(html.empty());
    REQUIRE(patch_flutter_web_file("index.html", html));

    // The bundled page names its scripts by content hash; follow index.html to them.
    const auto hashed = [&html](const std::string &prefix) {
        const size_t at  = html.find(prefix);
        const size_t end = html.find("?afc=1", at);
        REQUIRE(at != std::string::npos);
        REQUIRE(end != std::string::npos);
        return html.substr(at, end - at);
    };
    const std::string bootstrap_name = hashed("flutter_bootstrap.");
    const std::string main_name      = hashed("main.");

    std::string bootstrap = read_bundled(bootstrap_name);
    CHECK(patch_flutter_web_file(bootstrap_name, bootstrap));
    std::string main = read_bundled(main_name);
    CHECK(patch_flutter_web_file(main_name, main));
    CHECK(main.find("!!window.__afcDark") != std::string::npos);
}
