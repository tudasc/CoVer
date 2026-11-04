module;

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#include <json/json.h>

export module UserConfig;

// CoVer's per-user settings: a JSON value per key, kept in $XDG_CONFIG_HOME/cover/settings.json.
// The file is readable by the user only, so settings may hold secrets.
namespace UserConfig {

// The value as compact JSON, with UTF-8 unescaped
export std::string JsonString(Json::Value const& value) {
    static Json::StreamWriterBuilder const builder = [] {
        Json::StreamWriterBuilder b;
        b["indentation"] = "";
        b["emitUTF8"] = true;
        return b;
    }();
    return Json::writeString(builder, value);
}

std::filesystem::path Path() {
    char const* config = std::getenv("XDG_CONFIG_HOME");
    char const* home = std::getenv("HOME");
    std::filesystem::path dir = config && *config ? std::filesystem::path(config) : std::filesystem::path(home ? home : ".") / ".config";
    return dir / "cover" / "settings.json";
}

// All settings, an empty object if there are none yet
std::expected<Json::Value, std::string> Load() {
    std::filesystem::path path = Path();
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return Json::Value(Json::objectValue);
    if (st.st_mode & (S_IRWXG | S_IRWXO)) return std::unexpected(std::format("{} is accessible by other users, so it is not used. Restrict its permissions or delete it.", path.string()));
    std::ifstream file(path);
    Json::Value settings;
    std::string errs;
    if (!file || !Json::parseFromStream(Json::CharReaderBuilder(), file, &settings, &errs) || !settings.isObject())
        return std::unexpected(std::format("Could not read {}, it is no JSON object. Fix or delete it.", path.string()));
    return settings;
}

// Replaces the file with the settings. It is created with restricted permissions right away and renamed into place,
// so the content is never readable by others and a failed write leaves the previous settings.
std::expected<void, std::string> Save(Json::Value const& settings) {
    std::filesystem::path path = Path();
    std::error_code ec;
    bool created_dir = std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return std::unexpected(std::format("Could not create {}: {}", path.parent_path().string(), ec.message()));
    if (created_dir) std::filesystem::permissions(path.parent_path(), std::filesystem::perms::owner_all, ec);
    std::string content = JsonString(settings) + "\n";
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    std::filesystem::remove(tmp, ec);
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return std::unexpected(std::format("Could not create {}: {}", tmp.string(), std::strerror(errno)));
    bool ok = write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size());
    ok = close(fd) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::string error = std::format("Could not write {}: {}", path.string(), std::strerror(errno));
        std::filesystem::remove(tmp, ec);
        return std::unexpected(error);
    }
    return {};
}

// The setting stored under key, null if there is none or the settings cannot be read
export Json::Value Get(std::string const& key) {
    std::expected<Json::Value, std::string> settings = Load();
    return settings ? settings->get(key, Json::nullValue) : Json::nullValue;
}

// Stores value under key, keeping the other settings. Fails rather than replace settings that cannot be read.
export std::expected<void, std::string> Set(std::string const& key, Json::Value const& value) {
    std::expected<Json::Value, std::string> settings = Load();
    if (!settings) return std::unexpected(settings.error());
    (*settings)[key] = value;
    return Save(*settings);
}

} // namespace UserConfig
