#ifndef slic3r_GUI_AFCCloudUpload_hpp_
#define slic3r_GUI_AFCCloudUpload_hpp_

#include <cstddef>
#include <functional>
#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace GUI {

// Uploads a print to Snapmaker's cloud for a U1 the slicer cannot reach on the
// LAN, the way Snapmaker's own print popup does it away from home:
// /user/device/upload/create, a PUT of the file (in 5 MiB parts above that
// size) to the storage URLs it hands back, then /user/device/upload/completed.
// The U1 then fetches the file with server.files.start_cloud_print.
class AFCCloudUpload
{
public:
    struct Account
    {
        std::string api_base; // https://api.snapmaker.com/api, or the .cn host
        std::string token;    // the signed-in Snapmaker account's token
    };
    struct Result
    {
        std::string url;      // where the U1 downloads the file from
        std::string checksum; // base64 SHA-256 of the whole file
        size_t      size{0};
    };
    // Bytes sent so far and the file size; returning false cancels.
    using Progress = std::function<bool(size_t done, size_t total)>;

    // The account the slicer is signed in to. False when nobody is signed in. UI thread only.
    static bool signed_in_account(Account &account);

    // Blocking, for a worker thread. False with `error` set when it failed or was canceled.
    static bool upload(const Account &account, const boost::filesystem::path &gcode, const std::string &file_name,
                       const std::string &serial, const Progress &progress, Result &result, std::string &error);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AFCCloudUpload_hpp_
