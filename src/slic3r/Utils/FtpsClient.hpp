#ifndef slic3r_FtpsClient_hpp_
#define slic3r_FtpsClient_hpp_

// Minimal FTPS (FTP over implicit TLS, port 990) client built on libcurl.
//
// Bambu Lab printers expose an FTPS server (user "bblp", password = the printer
// access code) that holds the timelapse videos and the model (.3mf) files. This
// client is used to browse / download / delete those files when the printer is in
// "LAN only" mode, where the proprietary BambuSource tunnel used by
// PrinterFileSystem is not available. The printer presents a self-signed
// certificate, so peer/host verification is disabled (acceptable on the LAN).
//
// The class is intentionally free of any wxWidgets/GUI dependency so it can be
// driven from a worker thread and unit-reasoned about in isolation.

#include <cstdint>
#include <ctime>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace Slic3r {

struct FtpsEntry
{
    std::string   name;
    std::uint64_t size = 0;
    std::time_t   time = 0;
    bool          is_dir = false;
};

class FtpsClient
{
public:
    // progress(received, total) -> return false to abort the transfer.
    using ProgressFn = std::function<bool(std::uint64_t received, std::uint64_t total)>;

    FtpsClient(std::string host, int port, std::string user, std::string password);

    // List a directory (relative to the FTP login root, e.g. "sdcard/timelapse/").
    // Returns true on success; fills `out`. On failure fills `error`.
    bool list(const std::string &dir, std::vector<FtpsEntry> &out, std::string &error);

    // Download a remote file (path relative to root) into the given stream.
    bool download(const std::string &remote_path, std::ostream &os, const ProgressFn &progress, std::string &error);

    // Download a remote file fully into memory (used for thumbnail / 3mf parsing).
    bool download_to_string(const std::string &remote_path, std::string &out, const ProgressFn &progress, std::string &error);

    // Delete a remote file (path relative to root).
    bool remove(const std::string &remote_path, std::string &error);

private:
    // Builds "ftps://host:port/<percent-encoded path>" preserving '/' separators.
    std::string url_for(const std::string &path, bool is_dir) const;

    std::string m_host;
    int         m_port;
    std::string m_user;
    std::string m_password;
};

} // namespace Slic3r

#endif // slic3r_FtpsClient_hpp_
