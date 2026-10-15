#include "AFCCloudUpload.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AFCProtocol.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <iterator>

namespace Slic3r {
namespace GUI {

namespace {

using json = nlohmann::json;

// Snapmaker's popup sends files above 5 MiB in parts of this size.
constexpr size_t PART_SIZE = 5242880;

std::string sha256_base64(const char *data, size_t size)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(data), size, digest);
    std::string b64(boost::beast::detail::base64::encoded_size(sizeof(digest)), '\0');
    b64.resize(boost::beast::detail::base64::encode(&b64[0], digest, sizeof(digest)));
    return b64;
}

std::string text(const json &obj, const char *key)
{
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// One call to Snapmaker's cloud API. True when it answered code 200; `data` gets its data.
bool api_call(const AFCCloudUpload::Account &account, const std::string &path, const json &body, json &data, std::string &error)
{
    bool ok = false;
    auto http = Http::post(account.api_base + path);
    // The account token goes in raw, as in the slicer's other Snapmaker API calls.
    http.header("Content-Type", "application/json")
        .header("accept", "application/json")
        .header("Authorization", account.token)
        .set_post_body(body.dump())
        .timeout_connect(10)
        .timeout_max(60)
        .on_complete([&](std::string answer, unsigned status) {
            const json res = json::parse(answer, nullptr, false);
            auto code = res.is_object() ? res.find("code") : res.end();
            if (code != res.end() && code->is_number_integer() && code->get<int>() == 200) {
                ok   = true;
                auto it = res.find("data");
                data = it != res.end() ? *it : json();
                return;
            }
            const std::string msg = res.is_object() ? text(res, "msg") : std::string();
            error = path + ": " + (msg.empty() ? "HTTP " + std::to_string(status) + " " + answer.substr(0, 200) : msg) +
                    (code != res.end() ? " (code " + code->dump() + ")" : "");
        })
        .on_error([&](std::string answer, std::string err, unsigned status) {
            error = path + ": " + (err.empty() ? "HTTP " + std::to_string(status) : err) + (answer.empty() ? "" : " " + answer.substr(0, 200));
        })
        .perform_sync();
    return ok;
}

// PUT one piece of the file to a storage URL from upload/create. `etag` gets the
// ETag the storage answered with, quotes stripped.
bool put_piece(const std::string &url, const std::string &method, const char *data, size_t size, const std::string &checksum,
               const std::function<bool(size_t)> &sent, std::string &etag, std::string &error)
{
    bool        ok = false;
    std::string headers;
    auto        http = method == "POST" ? Http::post(url) : Http::put2(url);
    http.header("Content-Type", "application/octet-stream")
        .header("x-amz-checksum-sha256", checksum)
        .set_post_body(std::string(data, size))
        .timeout_connect(10)
        .on_header_callback([&](std::string all) { headers = std::move(all); })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            if (!sent(progress.ulnow))
                cancel = true;
        })
        .on_complete([&](std::string, unsigned) { ok = true; })
        .on_error([&](std::string answer, std::string err, unsigned status) {
            error = "file upload: " + (err.empty() ? "HTTP " + std::to_string(status) : err) + (answer.empty() ? "" : " " + answer.substr(0, 200));
        })
        .perform_sync();
    if (!ok)
        return false;
    // Headers of every response so far, redirects included: the last ETag wins.
    etag.clear();
    size_t start = 0;
    while (start < headers.size()) {
        size_t end = headers.find('\n', start);
        if (end == std::string::npos)
            end = headers.size();
        std::string line = headers.substr(start, end - start);
        start            = end + 1;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (name != "etag")
            continue;
        std::string value = line.substr(colon + 1);
        value.erase(std::remove_if(value.begin(), value.end(), [](char c) { return c == '"' || c == '\r' || c == ' ' || c == '\t'; }),
                    value.end());
        etag = value;
    }
    return true;
}

} // namespace

bool AFCCloudUpload::signed_in_account(Account &account)
{
    auto *user = wxGetApp().sm_get_userinfo();
    if (user == nullptr || !user->is_user_login() || user->get_user_token().empty())
        return false;
    const std::string country = wxGetApp().app_config ? wxGetApp().app_config->get_country_code() : std::string();
    account.api_base = country.find("CN") != std::string::npos ? "https://api.snapmaker.cn/api" : "https://api.snapmaker.com/api";
    account.token    = user->get_user_token();
    return true;
}

bool AFCCloudUpload::upload(const Account &account, const boost::filesystem::path &gcode, const std::string &file_name,
                            const std::string &serial, const Progress &progress, Result &result, std::string &error)
{
    error.clear();
    std::string data;
    {
        boost::filesystem::ifstream in(gcode, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (data.empty()) {
        error = "could not read " + gcode.string();
        return false;
    }
    result.size     = data.size();
    result.checksum = sha256_base64(data.data(), data.size());

    // The upload names the device it is for, as Snapmaker's popup does; without its id it goes out unnamed.
    json        device_id;
    json        devices;
    std::string list_error;
    if (api_call(account, "/user/device/list",
                 json{{"name", nullptr}, {"mac", nullptr}, {"sn", nullptr}, {"productName", nullptr},
                      {"productCode", nullptr}, {"pageIndex", nullptr}, {"pageRows", nullptr}},
                 devices, list_error))
        device_id = AFCProtocol::cloud_device_id(devices, serial);
    if (device_id.is_null())
        BOOST_LOG_TRIVIAL(warning) << "AFC print: no cloud id for the U1 " << serial << (list_error.empty() ? "" : " (" + list_error + ")");

    json create = {{"fileName", file_name}, {"checkSum", result.checksum}, {"contentType", "application/octet-stream"}};
    if (!device_id.is_null())
        create["deviceId"] = device_id;
    const bool multipart = data.size() > PART_SIZE;
    if (multipart) {
        json parts = json::array();
        for (size_t off = 0, number = 1; off < data.size(); off += PART_SIZE, ++number)
            parts.push_back({{"partNumber", number}, {"checkSum", sha256_base64(data.data() + off, std::min(PART_SIZE, data.size() - off))}});
        create["uploadParts"] = parts;
    }
    json created;
    if (!api_call(account, "/user/device/upload/create", create, created, error))
        return false;

    size_t done = 0;
    auto   sent = [&](size_t now) { return !progress || progress(done + now, data.size()); };
    auto   canceled = [&]() {
        if (error.empty())
            error = "canceled";
        return false;
    };

    json completed;
    if (created.is_object() && created.contains("url")) {
        std::string etag;
        if (!put_piece(text(created, "url"), text(created, "method"), data.data(), data.size(), result.checksum, sent, etag, error))
            return canceled();
        if (!api_call(account, "/user/device/upload/completed", create, completed, error))
            return false;
    } else {
        if (!created.is_object() || !created.contains("uploadParts") || !created["uploadParts"].is_array() || !multipart) {
            error = "/user/device/upload/create: no upload address in the answer";
            return false;
        }
        // upload/completed gets create's answer back with each part's ETag, as Snapmaker's popup sends it.
        json finish = created;
        finish.erase("method");
        finish.erase("contentType");
        finish["checkSum"] = result.checksum;
        finish["deviceId"] = device_id;
        finish["fileName"] = file_name;
        auto give_up = [&]() {
            if (finish.contains("uploadId")) {
                json        ignored;
                std::string cancel_error;
                api_call(account, "/user/device/upload/cancel", json{{"uploadId", finish["uploadId"]}}, ignored, cancel_error);
            }
            return false;
        };
        for (json &part : finish["uploadParts"]) {
            auto number = part.is_object() ? part.find("partNumber") : part.end();
            if (number == part.end() || !number->is_number_integer() || number->get<long long>() < 1 ||
                size_t(number->get<long long>() - 1) * PART_SIZE >= data.size()) {
                error = "/user/device/upload/create: a part without a valid number";
                return give_up();
            }
            const size_t off  = size_t(number->get<long long>() - 1) * PART_SIZE;
            const size_t len  = std::min(PART_SIZE, data.size() - off);
            std::string  sum  = text(part, "checkSum");
            if (sum.empty())
                sum = sha256_base64(data.data() + off, len);
            std::string etag;
            if (!put_piece(text(part, "url"), "PUT", data.data() + off, len, sum, sent, etag, error)) {
                canceled();
                return give_up();
            }
            if (etag.empty()) {
                error = "file upload: no ETag for part " + number->dump();
                return give_up();
            }
            part["etag"] = etag;
            done += len;
        }
        if (!api_call(account, "/user/device/upload/completed", finish, completed, error))
            return give_up();
    }
    result.url = AFCProtocol::cloud_upload_location(completed, multipart);
    if (result.url.empty()) {
        error = "/user/device/upload/completed: no file location in the answer";
        return false;
    }
    return true;
}

} // namespace GUI
} // namespace Slic3r
