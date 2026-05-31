#include "FtpsClient.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <curl/curl.h>

namespace Slic3r {

// ---------------------------------------------------------------------------
// libcurl callbacks
// ---------------------------------------------------------------------------

static size_t write_to_string_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t write_to_stream_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *os = static_cast<std::ostream *>(userdata);
    os->write(ptr, size * nmemb);
    return os->good() ? size * nmemb : 0;
}

struct ProgressCtx
{
    const FtpsClient::ProgressFn *fn;
};

static int xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
    auto *ctx = static_cast<ProgressCtx *>(clientp);
    if (ctx && ctx->fn && *ctx->fn) {
        if (!(*ctx->fn)(static_cast<std::uint64_t>(dlnow), static_cast<std::uint64_t>(dltotal)))
            return 1; // abort
    }
    return 0;
}

// ---------------------------------------------------------------------------
// LIST output parsing (Unix "ls -l" style)
// ---------------------------------------------------------------------------

static std::time_t parse_unix_date(const std::string &mon, const std::string &day, const std::string &year_or_time)
{
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int m = 0;
    for (; m < 12; ++m)
        if (boost::iequals(mon, months[m]))
            break;
    if (m == 12)
        return 0;

    std::tm tmv{};
    tmv.tm_mon  = m;
    tmv.tm_mday = std::atoi(day.c_str());
    tmv.tm_isdst = -1;

    if (year_or_time.find(':') != std::string::npos) {
        // "HH:MM" -> the year is implicit (current, or previous if the month is
        // in the future relative to now), matching the `ls` heuristic.
        std::time_t now = std::time(nullptr);
        std::tm     lt{};
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        int year = lt.tm_year + 1900;
        if (m > lt.tm_mon)
            --year;
        tmv.tm_year = year - 1900;
        tmv.tm_hour = std::atoi(year_or_time.c_str());
        auto colon  = year_or_time.find(':');
        tmv.tm_min  = std::atoi(year_or_time.c_str() + colon + 1);
    } else {
        tmv.tm_year = std::atoi(year_or_time.c_str()) - 1900;
    }
    std::time_t t = std::mktime(&tmv);
    return t < 0 ? 0 : t;
}

// Returns the offset of the start of the `field_index`-th whitespace separated
// field (0-based) in `line`, or std::string::npos.
static size_t nth_field_offset(const std::string &line, int field_index)
{
    int  field = 0;
    bool in_ws = true;
    for (size_t i = 0; i < line.size(); ++i) {
        bool ws = line[i] == ' ' || line[i] == '\t';
        if (in_ws && !ws) {
            if (field == field_index)
                return i;
            ++field;
        }
        in_ws = ws;
    }
    return std::string::npos;
}

static bool parse_list_line(const std::string &raw, FtpsEntry &e)
{
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (line.size() < 10)
        return false;

    std::istringstream       iss(line);
    std::vector<std::string> tok;
    std::string              t;
    while (iss >> t)
        tok.push_back(t);
    if (tok.size() < 9)
        return false;

    e.is_dir = (line[0] == 'd');
    try {
        e.size = std::stoull(tok[4]);
    } catch (...) {
        e.size = 0;
    }
    e.time = parse_unix_date(tok[5], tok[6], tok[7]);

    // Name is everything starting at the 9th field, so spaces in names survive.
    size_t name_off = nth_field_offset(line, 8);
    if (name_off == std::string::npos)
        return false;
    e.name = line.substr(name_off);
    // Symlinks: "name -> target"
    if (line[0] == 'l') {
        auto arrow = e.name.find(" -> ");
        if (arrow != std::string::npos)
            e.name = e.name.substr(0, arrow);
    }
    return !e.name.empty() && e.name != "." && e.name != "..";
}

// ---------------------------------------------------------------------------
// FtpsClient
// ---------------------------------------------------------------------------

FtpsClient::FtpsClient(std::string host, int port, std::string user, std::string password)
    : m_host(std::move(host)), m_port(port > 0 ? port : 990), m_user(std::move(user)), m_password(std::move(password))
{}

std::string FtpsClient::url_for(const std::string &path, bool is_dir) const
{
    std::string url = "ftps://" + m_host + ":" + std::to_string(m_port) + "/";
    // Percent-encode each segment but keep '/' separators intact.
    std::vector<std::string> segs;
    boost::split(segs, path, boost::is_any_of("/"));
    std::string encoded;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (segs[i].empty())
            continue;
        char *esc = curl_easy_escape(nullptr, segs[i].c_str(), (int) segs[i].size());
        if (esc) {
            if (!encoded.empty())
                encoded += "/";
            encoded += esc;
            curl_free(esc);
        }
    }
    url += encoded;
    if (is_dir && (url.empty() || url.back() != '/'))
        url += "/";
    return url;
}

static void set_common_opts(CURL *c, const std::string &userpwd)
{
    curl_easy_setopt(c, CURLOPT_USE_SSL, (long) CURLUSESSL_ALL);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);  // printer uses a self-signed cert
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_USERPWD, userpwd.c_str());
    curl_easy_setopt(c, CURLOPT_FTP_USE_EPSV, 1L);
    curl_easy_setopt(c, CURLOPT_FTPSSLAUTH, (long) CURLFTPAUTH_DEFAULT);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FTP_RESPONSE_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
}

bool FtpsClient::list(const std::string &dir, std::vector<FtpsEntry> &out, std::string &error)
{
    CURL *c = curl_easy_init();
    if (!c) {
        error = "curl init failed";
        return false;
    }
    std::string userpwd = m_user + ":" + m_password;
    std::string body;
    std::string url = url_for(dir, true);
    char        errbuf[CURL_ERROR_SIZE] = {0};

    set_common_opts(c, userpwd);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_string_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    // Default (no DIRLISTONLY) -> server returns a full LIST with size/date.

    CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (res != CURLE_OK) {
        error = errbuf[0] ? errbuf : curl_easy_strerror(res);
        BOOST_LOG_TRIVIAL(info) << "FtpsClient::list " << url << " failed: " << error;
        return false;
    }

    std::vector<std::string> lines;
    boost::split(lines, body, boost::is_any_of("\n"));
    bool any_parsed = false;
    for (auto &ln : lines) {
        if (ln.empty() || ln == "\r")
            continue;
        FtpsEntry e;
        if (parse_list_line(ln, e)) {
            out.push_back(std::move(e));
            any_parsed = true;
        }
    }

    // Fallback: server returned a name-only listing we couldn't parse as `ls -l`.
    if (!any_parsed && !body.empty()) {
        for (auto &ln : lines) {
            std::string name = ln;
            while (!name.empty() && (name.back() == '\r' || name.back() == '\n'))
                name.pop_back();
            if (name.empty() || name == "." || name == "..")
                continue;
            FtpsEntry e;
            e.name = name;
            out.push_back(std::move(e));
        }
    }
    return true;
}

bool FtpsClient::download(const std::string &remote_path, std::ostream &os, const ProgressFn &progress, std::string &error)
{
    CURL *c = curl_easy_init();
    if (!c) {
        error = "curl init failed";
        return false;
    }
    std::string userpwd = m_user + ":" + m_password;
    std::string url     = url_for(remote_path, false);
    char        errbuf[CURL_ERROR_SIZE] = {0};
    ProgressCtx pctx{&progress};

    set_common_opts(c, userpwd);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_stream_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &os);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    if (progress) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pctx);
    }

    CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (res != CURLE_OK) {
        error = errbuf[0] ? errbuf : curl_easy_strerror(res);
        return false;
    }
    return true;
}

bool FtpsClient::download_to_string(const std::string &remote_path, std::string &out, const ProgressFn &progress, std::string &error)
{
    std::ostringstream oss(std::ios::binary);
    if (!download(remote_path, oss, progress, error))
        return false;
    out = oss.str();
    return true;
}

bool FtpsClient::remove(const std::string &remote_path, std::string &error)
{
    CURL *c = curl_easy_init();
    if (!c) {
        error = "curl init failed";
        return false;
    }
    std::string userpwd = m_user + ":" + m_password;

    // DELE needs the directory URL + a "DELE <name>" quote command.
    auto slash = remote_path.find_last_of('/');
    std::string dir  = slash == std::string::npos ? std::string() : remote_path.substr(0, slash + 1);
    std::string name = slash == std::string::npos ? remote_path : remote_path.substr(slash + 1);
    std::string url  = url_for(dir, true);
    char        errbuf[CURL_ERROR_SIZE] = {0};

    struct curl_slist *cmds = nullptr;
    cmds = curl_slist_append(cmds, ("DELE " + name).c_str());

    set_common_opts(c, userpwd);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_QUOTE, cmds);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(cmds);
    curl_easy_cleanup(c);

    if (res != CURLE_OK) {
        error = errbuf[0] ? errbuf : curl_easy_strerror(res);
        return false;
    }
    return true;
}

} // namespace Slic3r
