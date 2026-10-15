#ifndef slic3r_GUI_McpServer_hpp_
#define slic3r_GUI_McpServer_hpp_

// In-process Model Context Protocol (MCP) server.
//
// Exposes Bambu Studio to AI agents (Claude Code, Claude Desktop, ...) over the
// MCP "Streamable HTTP" transport: JSON-RPC 2.0 messages POSTed to
// http://127.0.0.1:<port>/mcp. The server only listens on the loopback
// interface. Tool handlers touching the GUI/model are marshalled to the wx main
// thread, so they behave exactly like user actions (undo stack, refresh, ...).

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include "nlohmann/json.hpp"

class wxDialog;

namespace Slic3r { namespace GUI { namespace Mcp {

using json = nlohmann::json;

static constexpr uint16_t DEFAULT_PORT = 13619;

// Thrown by tool handlers: reported to the agent as a tool error (isError = true),
// not as a protocol error, so that the model can read the message and recover.
struct ToolError : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Result of a tool call: MCP "content" blocks.
struct ToolResult
{
    json content  = json::array();
    bool is_error = false;

    static ToolResult text(const std::string &text);
    // Pretty-printed JSON as a text block (what agents read best).
    static ToolResult data(const json &value);
    ToolResult       &add_text(const std::string &text);
    ToolResult       &add_image_png(const std::string &base64_png);
};

struct Tool
{
    std::string name;
    std::string description;
    json        input_schema = json{{"type", "object"}, {"properties", json::object()}};
    // When true the handler is invoked on the wx main thread. Long-running tools
    // (slicing, arranging, ...) set this to false and marshal fine-grained steps
    // themselves with run_on_gui(), so the GUI stays responsive while they wait.
    bool        on_gui_thread = true;
    bool        read_only     = false;
    bool        destructive   = false;
    std::function<ToolResult(const json &args)> handler;
};

class ToolRegistry
{
public:
    void        add(Tool tool);
    const Tool *find(const std::string &name) const;
    json        list_json() const;

private:
    std::vector<Tool>             m_tools;
    std::map<std::string, size_t> m_index;
};

// Defined in McpTools*.cpp
void register_project_tools(ToolRegistry &registry);
void register_object_tools(ToolRegistry &registry);
void register_config_tools(ToolRegistry &registry);
void register_paint_tools(ToolRegistry &registry);
void register_slice_tools(ToolRegistry &registry);
void register_device_tools(ToolRegistry &registry);

// --- Modal dialogs ---------------------------------------------------------------------------------------------------
// Tools reuse Bambu Studio functions that may ask the user something. While a tool
// runs, no modal dialog is ever displayed: a wxModalDialogHook answers it at once
// (informational dialogs: OK; questions: Cancel/No, i.e. the action is not done)
// and the dialog text is returned to the agent. Dialogs opened by the user
// outside of tool calls are not affected.

// Answer to give to some dialogs during the current tool call; return wxID_NONE to
// keep the default rule. Call it from a tool handler (any thread).
void set_dialog_override(std::function<int(wxDialog *)> fn);

// While alive, dialogs raised by background jobs started by the current tool call
// (arrange, orient, slicing of the next plates...) are answered too, even outside
// of run_on_gui() steps. Create it on the HTTP thread, around the wait for the job.
class JobDialogScope
{
public:
    JobDialogScope();
    ~JobDialogScope();
    JobDialogScope(const JobDialogScope &) = delete;
    JobDialogScope &operator=(const JobDialogScope &) = delete;
};

// Runs `fn` on the wx main thread and waits for its result. Exceptions thrown by
// `fn` are re-thrown in the caller. Executes inline when already on the main thread.
json run_on_gui(std::function<json()> fn, std::chrono::milliseconds timeout = std::chrono::seconds(60));

// Polls `predicate` (evaluated on the main thread) until it returns true or the
// timeout expires. Must NOT be called from the main thread. Returns the last value.
bool wait_on_gui(std::function<bool()> predicate, std::chrono::milliseconds timeout,
                 std::chrono::milliseconds period = std::chrono::milliseconds(250));

class McpServer
{
public:
    static McpServer &instance();

    // Starts listening on 127.0.0.1:port. When `token` is not empty, requests must
    // carry "Authorization: Bearer <token>".
    bool        start(uint16_t port, const std::string &token);
    void        stop();
    bool        is_running() const { return m_running.load(); }
    uint16_t    port() const { return m_port; }
    std::string url() const;

    // Starts/stops the server according to the "mcp_server_*" keys of AppConfig.
    static void apply_app_config();

    // Exposed for the HTTP sessions.
    struct HttpReply
    {
        unsigned                                         status = 200;
        std::string                                      body;
        std::string                                      content_type = "application/json";
        std::vector<std::pair<std::string, std::string>> headers;
    };
    HttpReply handle_http(const std::string &method, const std::string &target, const std::string &body,
                          const std::string &origin, const std::string &authorization, const std::string &protocol_version);

private:
    McpServer();
    ~McpServer();

    json handle_message(const json &msg);
    json handle_request(const std::string &method, const json &params);
    json call_tool(const json &params);

    void do_accept();
    void serve_connection(std::shared_ptr<boost::asio::ip::tcp::socket> socket);

    ToolRegistry m_registry;

    std::atomic_bool                                                                          m_running{false};
    uint16_t                                                                                  m_port{0};
    std::string                                                                               m_token;
    boost::asio::io_context                                                                   m_io{1};
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> m_guard;
    std::unique_ptr<boost::asio::ip::tcp::acceptor>                                           m_acceptor;
    std::thread                                                                               m_io_thread;

    std::mutex                                                     m_sockets_mutex;
    std::set<std::shared_ptr<boost::asio::ip::tcp::socket>>        m_sockets;
};

}}} // namespace Slic3r::GUI::Mcp

#endif // slic3r_GUI_McpServer_hpp_
