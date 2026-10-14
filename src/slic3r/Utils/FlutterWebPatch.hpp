#ifndef slic3r_FlutterWebPatch_hpp_
#define slic3r_FlutterWebPatch_hpp_

#include <string>

namespace Slic3r {

// Snapmaker's Flutter page (data_dir()/web/flutter_web, updated over the air)
// ships a dark theme but forces it off when it starts. The local web server
// passes the page's files through here so the device page follows the
// slicer's dark mode:
//  - index.html sets window.__afcDark when its query has dark_mode=1 and path=2,
//  - main.<hash>.js reads that flag instead of the hard-coded "light",
//  - the bootstrap and main script URLs get a query tag, so a copy the web view
//    cached before the rewrite (served "immutable") is not reused.
// A file whose pattern is not found (a Snapmaker update changed it) is left
// alone and the page stays light.
//
// basename is the served file's lowercased name. Returns true if content changed.
bool patch_flutter_web_file(const std::string &basename, std::string &content);

} // namespace Slic3r

#endif
