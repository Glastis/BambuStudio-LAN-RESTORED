#include "McpServer.hpp"

#include <future>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/log/trivial.hpp>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/html/htmlwin.h>
#include <wx/modalhook.h>
#include <wx/msgdlg.h>
#include <wx/stattext.h>
#include <wx/thread.h>

#include "libslic3r_version.h"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/ObjColorDialog.hpp"
#include "slic3r/GUI/ParamsPanel.hpp"
#include "slic3r/GUI/StepMeshDialog.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace beast = boost::beast;
namespace http  = boost::beast::http;
using tcp       = boost::asio::ip::tcp;

static std::atomic_bool g_shutting_down{false};

static const char *SUPPORTED_PROTOCOL_VERSIONS[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

static const char *SERVER_INSTRUCTIONS =
    "This server drives a running Bambu Studio instance (3D-printing slicer). Every tool acts on the live GUI, "
    "exactly like a user would, so changes are visible on screen and can be undone with the `undo` tool.\n"
    "Conventions:\n"
    "- Lengths are millimetres, angles are degrees, coordinates are world coordinates of the build area "
    "(plate 0 starts at the origin, other plates are offset; use `list_plates` to get their origin/size).\n"
    "- Objects, instances, volumes and plates are 0-based indices as returned by `list_objects` / `list_plates`. "
    "Filament slots (aka extruders) are 1-based, as in the Bambu Studio sidebar.\n"
    "- Settings use the slicer configuration keys (e.g. `enable_support`, `layer_height`, `sparse_infill_density`). "
    "Use `search_settings` to discover keys and their allowed values, `get_settings` to read them and `set_settings` "
    "to change them globally or per object/part.\n"
    "- Tools never display dialogs. Choices that Bambu Studio would ask the user are tool arguments (e.g. "
    "`discard_changes`, `unsaved_changes`, `run_post_process_scripts`); any remaining prompt is answered automatically "
    "(OK for information, Cancel/No for questions) and its text is returned in the tool result.\n"
    "Typical workflow: `get_app_state` -> `list_objects`/`list_plates` -> edit -> `screenshot` to check visually -> "
    "`slice` -> `save_project`.";

// ---------------------------------------------------------------------------------------------------------------------
// ToolResult / ToolRegistry
// ---------------------------------------------------------------------------------------------------------------------

ToolResult ToolResult::text(const std::string &text)
{
    ToolResult r;
    r.add_text(text);
    return r;
}

ToolResult ToolResult::data(const json &value) { return text(value.dump(2)); }

ToolResult &ToolResult::add_text(const std::string &text)
{
    content.push_back({{"type", "text"}, {"text", text}});
    return *this;
}

ToolResult &ToolResult::add_image_png(const std::string &base64_png)
{
    content.push_back({{"type", "image"}, {"data", base64_png}, {"mimeType", "image/png"}});
    return *this;
}

void ToolRegistry::add(Tool tool)
{
    auto it = m_index.find(tool.name);
    if (it != m_index.end()) {
        m_tools[it->second] = std::move(tool);
        return;
    }
    m_index[tool.name] = m_tools.size();
    m_tools.emplace_back(std::move(tool));
}

const Tool *ToolRegistry::find(const std::string &name) const
{
    auto it = m_index.find(name);
    return it == m_index.end() ? nullptr : &m_tools[it->second];
}

json ToolRegistry::list_json() const
{
    json tools = json::array();
    for (const Tool &t : m_tools) {
        json annotations = {{"readOnlyHint", t.read_only}};
        if (!t.read_only) annotations["destructiveHint"] = t.destructive;
        tools.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}, {"annotations", annotations}});
    }
    return tools;
}

// ---------------------------------------------------------------------------------------------------------------------
// Modal dialogs raised during a tool call
// ---------------------------------------------------------------------------------------------------------------------

struct DialogContext
{
    std::mutex                       mutex;
    json                             raised   = json::array();
    bool                             declined = false;
    std::function<int(wxDialog *)>   override_fn;
};

// Context of the tool call served by the current HTTP thread.
static thread_local std::shared_ptr<DialogContext> t_request_ctx;
// Context of the tool step currently executed on the main thread (main thread only).
static std::shared_ptr<DialogContext> g_active_ctx;
// Context of a tool call waiting for its background job (main thread only).
static std::shared_ptr<DialogContext> g_job_ctx;

void set_dialog_override(std::function<int(wxDialog *)> fn)
{
    std::shared_ptr<DialogContext> ctx = wxThread::IsMain() ? g_active_ctx : t_request_ctx;
    if (!ctx) return;
    std::lock_guard<std::mutex> lock(ctx->mutex);
    ctx->override_fn = std::move(fn);
}

static void collect_dialog_text(wxWindow *window, std::vector<std::string> &out)
{
    for (wxWindow *child : window->GetChildren()) {
        if (auto *label = dynamic_cast<wxStaticText *>(child)) {
            if (!label->GetLabel().empty()) out.push_back(into_u8(label->GetLabel()));
        } else if (auto *html = dynamic_cast<wxHtmlWindow *>(child)) {
            if (!html->ToText().empty()) out.push_back(into_u8(html->ToText()));
        }
        collect_dialog_text(child, out);
    }
}

static std::string dialog_text(wxDialog *dialog)
{
    std::vector<std::string> parts;
    if (auto *msg = dynamic_cast<wxMessageDialogBase *>(dialog)) {
        parts.push_back(into_u8(msg->GetMessage()));
        if (!msg->GetExtendedMessage().empty()) parts.push_back(into_u8(msg->GetExtendedMessage()));
    }
    collect_dialog_text(dialog, parts);
    std::string text = boost::algorithm::join(parts, "\n");
    if (text.size() > 2000) text = text.substr(0, 2000) + "...";
    return text;
}

static const char *answer_name(int id)
{
    switch (id) {
    case wxID_OK: return "OK";
    case wxID_YES: return "Yes";
    case wxID_NO: return "No";
    case wxID_CANCEL: return "Cancel";
    default: return "other";
    }
}

// Default answer: acknowledge informational dialogs, decline questions.
static int default_answer(wxDialog *dialog, bool &declined)
{
    declined = false;
    // Known dialogs with a deterministic, harmless answer.
    if (dynamic_cast<::StepMeshDialog *>(dialog)) return wxID_OK;            // default STEP meshing precision
    if (dynamic_cast<::ObjColorDialog *>(dialog)) return wxID_CANCEL;        // no automatic colour mapping
    if (dynamic_cast<Newer3mfVersionDialog *>(dialog)) return wxID_CANCEL; // "Skip" the update
    if (dynamic_cast<TipsDialog *>(dialog)) return wxID_OK;
    if (dynamic_cast<InfoDialog *>(dialog)) return wxID_OK;
    if (dynamic_cast<PostProcessScriptDialog *>(dialog)) return wxID_NO;   // never run embedded scripts unless asked

    if (auto *msg = dynamic_cast<wxMessageDialogBase *>(dialog)) {
        const long style = msg->GetMessageDialogStyle();
        if (style & wxCANCEL) { declined = true; return wxID_CANCEL; }
        if (style & wxNO) { declined = true; return wxID_NO; }
        return (style & wxYES) ? wxID_YES : wxID_OK;
    }
    if (dialog->FindWindow(wxID_CANCEL)) { declined = true; return wxID_CANCEL; }
    if (dialog->FindWindow(wxID_NO)) { declined = true; return wxID_NO; }
    if (dialog->FindWindow(wxID_OK)) return wxID_OK;
    if (dialog->FindWindow(wxID_YES)) return wxID_YES;
    declined = true;
    return wxID_CANCEL;
}

class McpDialogHook : public wxModalDialogHook
{
protected:
    int Enter(wxDialog *dialog) override
    {
        std::shared_ptr<DialogContext> ctx = g_active_ctx ? g_active_ctx : g_job_ctx;
        if (!ctx)
            return wxID_NONE; // not raised by a tool: show it to the user normally

        int  answer   = wxID_NONE;
        bool declined = false;
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            if (ctx->override_fn) answer = ctx->override_fn(dialog);
        }
        if (answer == wxID_NONE) answer = default_answer(dialog, declined);

        json entry = {{"title", into_u8(dialog->GetTitle())}, {"text", dialog_text(dialog)}, {"answer", answer_name(answer)}};
        BOOST_LOG_TRIVIAL(info) << "[MCP] dialog answered automatically: " << entry.dump();
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->raised.push_back(std::move(entry));
        ctx->declined |= declined;
        return answer;
    }
};

static McpDialogHook g_dialog_hook;

JobDialogScope::JobDialogScope()
{
    run_on_gui([]() {
        g_job_ctx = g_active_ctx;
        return json();
    });
}

JobDialogScope::~JobDialogScope()
{
    try {
        run_on_gui([]() {
            g_job_ctx.reset();
            return json();
        });
    } catch (...) {}
}

// ---------------------------------------------------------------------------------------------------------------------
// Main thread marshalling
// ---------------------------------------------------------------------------------------------------------------------

json run_on_gui(std::function<json()> fn, std::chrono::milliseconds timeout)
{
    if (wxThread::IsMain())
        return fn();

    if (wxTheApp == nullptr || g_shutting_down.load())
        throw ToolError("Bambu Studio is shutting down.");

    std::shared_ptr<DialogContext> ctx = t_request_ctx ? t_request_ctx : std::make_shared<DialogContext>();
    auto promise = std::make_shared<std::promise<json>>();
    auto future  = promise->get_future();
    wxTheApp->CallAfter([promise, ctx, fn = std::move(fn)]() {
        // Every modal dialog opened while `fn` runs is answered by McpDialogHook.
        std::shared_ptr<DialogContext> previous = g_active_ctx;
        g_active_ctx                            = ctx;
        try {
            promise->set_value(fn());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
        g_active_ctx = previous;
    });

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (g_shutting_down.load())
            throw ToolError("Bambu Studio is shutting down.");
        if (std::chrono::steady_clock::now() >= deadline)
            throw ToolError("Timed out waiting for the Bambu Studio GUI thread (busy, or a dialog opened by the user is still open).");
    }
    return future.get();
}

bool wait_on_gui(std::function<bool()> predicate, std::chrono::milliseconds timeout, std::chrono::milliseconds period)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (run_on_gui([&predicate]() { return json(predicate()); }).get<bool>())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(period);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// McpServer
// ---------------------------------------------------------------------------------------------------------------------

McpServer &McpServer::instance()
{
    static McpServer server;
    return server;
}

McpServer::McpServer()
{
    register_project_tools(m_registry);
    register_object_tools(m_registry);
    register_config_tools(m_registry);
    register_paint_tools(m_registry);
    register_slice_tools(m_registry);
    register_device_tools(m_registry);
}

McpServer::~McpServer() { stop(); }

std::string McpServer::url() const { return "http://127.0.0.1:" + std::to_string(m_port) + "/mcp"; }

void McpServer::apply_app_config()
{
    AppConfig *config = wxGetApp().app_config;
    if (config == nullptr)
        return;

    McpServer  &server  = instance();
    const bool  enabled = config->get_bool("mcp_server_enabled");
    int         port    = DEFAULT_PORT;
    try {
        const std::string p = config->get("mcp_server_port");
        if (!p.empty()) port = std::stoi(p);
    } catch (...) {}
    if (port <= 0 || port > 65535) port = DEFAULT_PORT;
    const std::string token = config->get("mcp_server_token");

    if (server.is_running() && (!enabled || server.m_port != port || server.m_token != token))
        server.stop();
    if (enabled && !server.is_running())
        server.start(uint16_t(port), token);
}

bool McpServer::start(uint16_t port, const std::string &token)
{
    if (m_running.load())
        return true;

    g_shutting_down = false;
    m_port          = port;
    m_token         = token;

    boost::system::error_code ec;
    m_acceptor = std::make_unique<tcp::acceptor>(m_io);
    tcp::endpoint ep{boost::asio::ip::make_address("127.0.0.1"), port};
    m_acceptor->open(ep.protocol(), ec);
    if (!ec) m_acceptor->set_option(tcp::acceptor::reuse_address(true), ec);
    if (!ec) m_acceptor->bind(ep, ec);
    if (!ec) m_acceptor->listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "[MCP] cannot listen on 127.0.0.1:" << port << ": " << ec.message();
        m_acceptor.reset();
        return false;
    }

    m_running = true;
    g_dialog_hook.Register();
    m_guard   = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(m_io.get_executor());
    do_accept();
    m_io_thread = std::thread([this] { m_io.run(); });
    BOOST_LOG_TRIVIAL(info) << "[MCP] server listening on " << url();
    return true;
}

void McpServer::stop()
{
    if (!m_running.exchange(false))
        return;

    g_shutting_down = true;
    boost::system::error_code ec;
    if (m_acceptor) m_acceptor->close(ec);
    {
        // Unblock the connection threads waiting in read().
        std::lock_guard<std::mutex> lock(m_sockets_mutex);
        for (auto &s : m_sockets) s->shutdown(tcp::socket::shutdown_both, ec);
    }
    m_guard.reset();
    m_io.stop();
    if (m_io_thread.joinable()) m_io_thread.join();
    m_io.restart();
    m_acceptor.reset();
    g_dialog_hook.Unregister();
    BOOST_LOG_TRIVIAL(info) << "[MCP] server stopped";
}

void McpServer::do_accept()
{
    auto socket = std::make_shared<tcp::socket>(m_io);
    m_acceptor->async_accept(*socket, [this, socket](boost::system::error_code ec) {
        if (!m_running.load())
            return;
        if (!ec) {
            // One thread per connection: a tool call may block for minutes (slicing),
            // and must not prevent other requests from being served.
            std::thread([this, socket] { serve_connection(socket); }).detach();
        }
        do_accept();
    });
}

void McpServer::serve_connection(std::shared_ptr<tcp::socket> socket)
{
    {
        std::lock_guard<std::mutex> lock(m_sockets_mutex);
        m_sockets.insert(socket);
    }

    beast::flat_buffer buffer;
    for (;;) {
        beast::error_code                       ec;
        http::request_parser<http::string_body> parser;
        parser.body_limit(64 * 1024 * 1024);
        http::read(*socket, buffer, parser, ec);
        if (ec)
            break;

        const auto &req = parser.get();
        auto        header = [&req](http::field f) { auto it = req.find(f); return it == req.end() ? std::string() : std::string(it->value()); };
        std::string protocol_version;
        if (auto it = req.find("MCP-Protocol-Version"); it != req.end()) protocol_version = std::string(it->value());

        HttpReply reply;
        try {
            reply = handle_http(std::string(req.method_string()), std::string(req.target()), req.body(), header(http::field::origin),
                                header(http::field::authorization), protocol_version);
        } catch (const std::exception &e) {
            reply.status = 500;
            reply.body   = json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32603}, {"message", e.what()}}}}.dump();
        }

        http::response<http::string_body> res{static_cast<http::status>(reply.status), req.version()};
        res.set(http::field::server, "BambuStudio-MCP");
        if (!reply.body.empty()) res.set(http::field::content_type, reply.content_type);
        for (const auto &h : reply.headers) res.set(h.first, h.second);
        res.keep_alive(req.keep_alive());
        res.body() = std::move(reply.body);
        res.prepare_payload();
        http::write(*socket, res, ec);
        if (ec || !res.keep_alive())
            break;
    }

    beast::error_code ignored;
    socket->shutdown(tcp::socket::shutdown_both, ignored);
    socket->close(ignored);
    std::lock_guard<std::mutex> lock(m_sockets_mutex);
    m_sockets.erase(socket);
}

static bool is_local_origin(const std::string &origin)
{
    // Protection against DNS rebinding: browsers always send Origin, local agents do not.
    if (origin.empty() || origin == "null")
        return origin.empty();
    std::string host = origin;
    if (auto p = host.find("://"); p != std::string::npos) host = host.substr(p + 3);
    if (auto p = host.find('/'); p != std::string::npos) host = host.substr(0, p);
    if (!host.empty() && host.front() == '[') {
        auto p = host.find(']');
        host   = host.substr(0, p == std::string::npos ? host.size() : p + 1);
    } else if (auto p = host.rfind(':'); p != std::string::npos)
        host = host.substr(0, p);
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

McpServer::HttpReply McpServer::handle_http(const std::string &method, const std::string &target, const std::string &body,
                                            const std::string &origin, const std::string &authorization, const std::string &protocol_version)
{
    HttpReply reply;
    auto      error_reply = [&reply](unsigned status, const std::string &msg) {
        reply.status = status;
        reply.body   = json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32000}, {"message", msg}}}}.dump();
        return reply;
    };

    if (!is_local_origin(origin))
        return error_reply(403, "Forbidden origin");
    if (!m_token.empty() && authorization != "Bearer " + m_token)
        return error_reply(401, "Missing or invalid bearer token");

    std::string path = target.substr(0, target.find('?'));
    if (path != "/mcp" && path != "/mcp/" && path != "/")
        return error_reply(404, "Not found, the MCP endpoint is /mcp");

    if (method == "GET") {
        // No server-initiated SSE stream.
        reply.status = 405;
        reply.headers.emplace_back("Allow", "POST, DELETE");
        return reply;
    }
    if (method == "DELETE") {
        reply.status = 200;
        return reply;
    }
    if (method != "POST") {
        reply.status = 405;
        reply.headers.emplace_back("Allow", "POST, DELETE");
        return reply;
    }

    json msg = json::parse(body, nullptr, false);
    if (msg.is_discarded()) {
        reply.status = 400;
        reply.body   = json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "Parse error"}}}}.dump();
        return reply;
    }

    json out;
    if (msg.is_array()) {
        out = json::array();
        for (const json &m : msg) {
            json r = handle_message(m);
            if (!r.is_null()) out.push_back(std::move(r));
        }
        if (out.empty()) out = nullptr;
    } else {
        out = handle_message(msg);
    }

    if (out.is_null()) {
        // Only notifications / responses: accepted, nothing to return.
        reply.status = 202;
        return reply;
    }
    reply.body = out.dump();
    return reply;
}

json McpServer::handle_message(const json &msg)
{
    if (!msg.is_object() || !msg.contains("method"))
        return nullptr; // response to a server request (we never send any) or garbage

    const bool  is_request = msg.contains("id");
    const json  id         = is_request ? msg["id"] : json(nullptr);
    std::string method     = msg["method"].is_string() ? msg["method"].get<std::string>() : std::string();
    const json  params     = msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();

    if (!is_request)
        return nullptr; // notifications/initialized, notifications/cancelled, ...

    try {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", handle_request(method, params)}};
    } catch (const json &rpc_error) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", rpc_error}};
    } catch (const std::exception &e) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32603}, {"message", e.what()}}}};
    }
}

json McpServer::handle_request(const std::string &method, const json &params)
{
    if (method == "initialize") {
        std::string requested = params.value("protocolVersion", std::string());
        std::string version   = SUPPORTED_PROTOCOL_VERSIONS[0];
        for (const char *v : SUPPORTED_PROTOCOL_VERSIONS)
            if (requested == v) version = v;
        return json{{"protocolVersion", version},
                    {"capabilities", {{"tools", {{"listChanged", false}}}}},
                    {"serverInfo", {{"name", "bambustudio"}, {"title", "Bambu Studio"}, {"version", SLIC3R_VERSION}}},
                    {"instructions", SERVER_INSTRUCTIONS}};
    }
    if (method == "ping" || method == "logging/setLevel")
        return json::object();
    if (method == "tools/list")
        return json{{"tools", m_registry.list_json()}};
    if (method == "tools/call")
        return call_tool(params);
    if (method == "resources/list")
        return json{{"resources", json::array()}};
    if (method == "resources/templates/list")
        return json{{"resourceTemplates", json::array()}};
    if (method == "prompts/list")
        return json{{"prompts", json::array()}};

    throw json{{"code", -32601}, {"message", "Method not found: " + method}};
}

json McpServer::call_tool(const json &params)
{
    const std::string name = params.value("name", std::string());
    const Tool       *tool = m_registry.find(name);
    if (tool == nullptr)
        throw json{{"code", -32602}, {"message", "Unknown tool: " + name}};

    json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
    BOOST_LOG_TRIVIAL(info) << "[MCP] tools/call " << name << " " << args.dump();

    // Collects the dialogs answered on behalf of the user during this call.
    auto dialogs  = std::make_shared<DialogContext>();
    t_request_ctx = dialogs;

    ToolResult result;
    try {
        if (tool->on_gui_thread) {
            auto handler = tool->handler;
            json r       = run_on_gui([handler, args]() {
                ToolResult tr = handler(args);
                return json{{"content", tr.content}, {"isError", tr.is_error}};
            });
            result.content  = r["content"];
            result.is_error = r["isError"].get<bool>();
        } else {
            result = tool->handler(args);
        }
    } catch (const ToolError &e) {
        result          = ToolResult::text(e.what());
        result.is_error = true;
    } catch (const json::exception &e) {
        result          = ToolResult::text(std::string("Invalid arguments: ") + e.what());
        result.is_error = true;
    } catch (const std::exception &e) {
        result          = ToolResult::text(std::string("Error: ") + e.what());
        result.is_error = true;
    }
    t_request_ctx.reset();

    {
        std::lock_guard<std::mutex> lock(dialogs->mutex);
        if (!dialogs->raised.empty()) {
            result.add_text("Bambu Studio raised dialog(s) during this call; they were not shown and were answered automatically:\n" +
                            dialogs->raised.dump(2));
            if (dialogs->declined) {
                result.is_error = true;
                result.add_text("At least one question was answered Cancel/No, so the requested action was probably not (fully) performed. "
                                "Read the dialog text above and use the tool arguments to make the choice explicitly.");
            }
        }
    }
    if (result.is_error)
        BOOST_LOG_TRIVIAL(warning) << "[MCP] " << name << " failed: " << result.content.dump();
    return json{{"content", result.content}, {"isError", result.is_error}};
}

}}} // namespace Slic3r::GUI::Mcp
