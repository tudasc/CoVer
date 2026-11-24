module;

#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json/json.h>

export module MCPServer;
import std;
import httplib;
import TraceTools;
import UserConfig;

// Serves the trace tools to MCP clients (e.g. coding agents) over the Streamable HTTP transport, on localhost only.
// The server is stateless and answers with plain JSON. Tool calls are executed on the UI thread, see ProcessPending.
namespace MCPServer {

using TraceTools::ToolResult;
using TraceTools::ToolSpec;
using UserConfig::JsonString;

export constexpr int DEFAULT_PORT = 7341;

// For tool calls while CoVer exits, it may restart for a reanalysis
export std::string const RESTARTING = "CoVer is restarting for a reanalysis (or was closed). If it restarts, wait a few seconds and then try again.";

std::string const INSTRUCTIONS = "Tools for CoVer's interactive mode, in which contract violations reported by CoVer's static analysis are inspected and false positives "
                                 "are fixed with annotations. Call get_debug_context first: it explains how to interpret traces and annotations, how to work through "
                                 "the reports and reanalyse, and the current state.";

std::set<std::string> const PROTOCOL_VERSIONS = {"2025-11-25", "2025-06-18", "2025-03-26"};
std::string const DEFAULT_PROTOCOL_VERSION = "2025-06-18";

constexpr std::size_t MAX_BODY_SIZE = 16 * 1024 * 1024;
constexpr std::time_t TIMEOUT_SECONDS = 10; // For reading requests and writing responses

export struct Activity {
    std::string summary;
    bool error;
};

struct Job {
    std::string tool;
    Json::Value args;
    std::optional<Json::Value> id; // JSON-RPC id, if the response can be deferred (see Server::DeferResponse): not for calls in a batch
    int local_port; // Of the connection the call came in on
    int remote_port;
    std::promise<std::optional<ToolResult>> result; // Empty if deferred
};

// The response to a tool call that is sent later, by the next run of CoVer after it re-executed itself: the connection stays open
// across exec, see Server::DeferResponse
export struct Deferred {
    int fd;
    Json::Value id;
};

// The connected socket of this process from remote_port to local_port, -1 if there is none
int FindConnection(int local_port, int remote_port) {
    std::error_code ec;
    for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
        std::string name = entry.path().filename().string();
        int fd = -1;
        if (std::from_chars(name.data(), name.data() + name.size(), fd).ec != std::errc()) continue;
        sockaddr_in local{}, remote{};
        socklen_t local_len = sizeof(local), remote_len = sizeof(remote);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_len) != 0 || local.sin_family != AF_INET) continue;
        if (getpeername(fd, reinterpret_cast<sockaddr*>(&remote), &remote_len) != 0) continue;
        if (ntohs(local.sin_port) == local_port && ntohs(remote.sin_port) == remote_port) return fd;
    }
    return -1;
}

Json::Value ToolCallResponse(Json::Value const& id, ToolResult const& result) {
    Json::Value response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    Json::Value content;
    content["type"] = "text";
    content["text"] = result.text;
    response["result"]["content"].append(content);
    response["result"]["isError"] = result.error;
    return response;
}

// For the next run of CoVer, which takes it with ParseDeferred
export std::string Serialize(Deferred const& deferred) {
    return std::format("{} {} {}", getpid(), deferred.fd, JsonString(deferred.id));
}

// The deferred response of a previous run of this process, i.e. before it re-executed itself. Empty if text was written by another
// process, e.g. if it is left over from a run that was killed: its file descriptors are gone.
export std::optional<Deferred> ParseDeferred(std::string const& text) {
    std::istringstream in(text);
    int pid = 0;
    Deferred deferred;
    if (!(in >> pid >> deferred.fd) || pid != getpid()) return std::nullopt;
    std::string errs;
    if (!Json::parseFromStream(Json::CharReaderBuilder(), in, &deferred.id, &errs)) return std::nullopt;
    fcntl(deferred.fd, F_SETFD, FD_CLOEXEC); // Not passed on again
    return deferred;
}

// Sends the response of a deferred tool call and closes its connection
export void SendDeferred(Deferred const& deferred, ToolResult const& result) {
    std::string body = JsonString(ToolCallResponse(deferred.id, result));
    std::string message = std::format("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.size(), body);
    for (std::size_t sent = 0; sent < message.size();) {
        ssize_t n = send(deferred.fd, message.data() + sent, message.size() - sent, MSG_NOSIGNAL); // The client may be gone
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += n;
    }
    close(deferred.fd);
}

// Comparison taking the same time for all inputs of the same length
bool SecretEquals(std::string const& a, std::string const& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

Json::Value RpcError(Json::Value const& id, int code, std::string const& message) {
    Json::Value response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"]["code"] = code;
    response["error"]["message"] = message;
    return response;
}

void SetJson(httplib::Response& res, int status, Json::Value const& body) {
    res.status = status;
    res.set_content(JsonString(body), "application/json");
}

export class Server {
    public:
        // wake is called from a server thread whenever a tool call waits for ProcessPending
        Server(int port, std::string token, std::vector<ToolSpec> specs, std::function<void()> wake) : port{port}, token{std::move(token)}, specs{std::move(specs)}, wake{std::move(wake)} {
            http.set_read_timeout(TIMEOUT_SECONDS, 0);
            http.set_write_timeout(TIMEOUT_SECONDS, 0);
            http.set_payload_max_length(MAX_BODY_SIZE);
            http.set_pre_routing_handler([this](httplib::Request const& req, httplib::Response& res) { return CheckRequest(req, res); });
            http.Post("/mcp", [this](httplib::Request const& req, httplib::Response& res) { HandleRequest(req, res); });
            // Errors detected by httplib itself (malformed or oversized requests) get a JSON-RPC error as well
            http.set_error_handler([](httplib::Request const&, httplib::Response& res) {
                if (!res.body.empty()) return httplib::Server::HandlerResponse::Unhandled;
                SetJson(res, res.status, RpcError(Json::Value(), -32600, std::format("Invalid HTTP request ({})", httplib::status_message(res.status))));
                return httplib::Server::HandlerResponse::Handled;
            });
            errno = 0;
            if (!http.bind_to_port("127.0.0.1", port)) { // Never reachable from other hosts
                error = std::format("Could not listen on 127.0.0.1:{}: {}", port, errno ? std::strerror(errno) : "unknown error");
                return;
            }
            thread = std::thread([this] { http.listen_after_bind(); });
        }

        ~Server() {
            stopping = true; // Before stopping the server, so tool calls waiting for the UI return
            http.stop();
            if (thread.joinable()) thread.join();
            FailPending(RESTARTING);
        }

        Server(Server const&) = delete;
        Server& operator=(Server const&) = delete;

        bool Running() const { return thread.joinable(); }
        std::string const& Error() const { return error; }
        std::string Url() const { return std::format("http://127.0.0.1:{}/mcp", port); }
        std::string AuthHeader() const { return "Authorization: Bearer " + token; }

        // Whether tool calls are executed, otherwise they fail with the given message. UI thread only.
        void SetAccepting(bool accept, std::string const& refusal_message = "") {
            {
                std::lock_guard<std::mutex> lock(mutex);
                accepting = accept;
                refusal = refusal_message;
            }
            if (!accept) FailPending(refusal_message);
        }

        // Runs the waiting tool calls with the given dispatcher. UI thread only.
        void ProcessPending(std::function<ToolResult(std::string const&, Json::Value const&)> const& run, std::function<void(Activity const&)> const& on_done) {
            std::vector<std::shared_ptr<Job>> jobs;
            {
                std::lock_guard<std::mutex> lock(mutex);
                jobs.swap(queue);
            }
            for (std::shared_ptr<Job> const& job : jobs) {
                running = job.get();
                ToolResult result = run(job->tool, job->args);
                running = nullptr;
                bool defer = deferred && deferred_job == job.get();
                job->result.set_value(defer ? std::nullopt : std::optional<ToolResult>(result));
                if (on_done) on_done({result.summary, result.error});
            }
        }

        // Called by a tool while it runs: its result is not sent, the connection is kept open across exec instead, for the next run of
        // CoVer to send the response with SendDeferred (see Deferred). False if that is not possible, e.g. for a call in a batch:
        // then the tool's result is sent as usual. UI thread only.
        bool DeferResponse() {
            if (!running || !running->id || deferred) return false;
            int fd = FindConnection(running->local_port, running->remote_port);
            if (fd < 0 || fcntl(fd, F_SETFD, 0) == -1) return false; // Clears FD_CLOEXEC, set by httplib
            deferred = Deferred{fd, *running->id};
            deferred_job = running;
            return true;
        }

        // The deferred response, see DeferResponse. UI thread only.
        std::optional<Deferred> const& DeferredResponse() const { return deferred; }

    private:
        int port;
        std::string token;
        std::vector<ToolSpec> specs;
        std::function<void()> wake;
        std::string error;
        httplib::Server http;
        std::thread thread;
        std::atomic<bool> stopping = false;
        std::mutex mutex; // Guards the members below
        bool accepting = false;
        std::string refusal = "CoVer is not accepting tool calls";
        std::vector<std::shared_ptr<Job>> queue;
        // UI thread only
        Job const* running = nullptr; // The job of the tool being run by ProcessPending
        std::optional<Deferred> deferred;
        Job const* deferred_job = nullptr;

        void FailPending(std::string const& message) {
            std::vector<std::shared_ptr<Job>> jobs;
            {
                std::lock_guard<std::mutex> lock(mutex);
                jobs.swap(queue);
            }
            for (std::shared_ptr<Job> const& job : jobs) job->result.set_value(ToolResult{message, true, job->tool});
        }

        // Runs before routing, answers requests that must not reach the endpoint
        httplib::Server::HandlerResponse CheckRequest(httplib::Request const& req, httplib::Response& res) {
            auto refuse = [&](int status, std::string const& message) {
                SetJson(res, status, RpcError(Json::Value(), -32600, message));
                return httplib::Server::HandlerResponse::Handled;
            };
            if (req.path != "/mcp") return refuse(404, "The MCP endpoint is /mcp, you tried: " + req.path);
            // Protection against DNS rebinding, browsers must not be able to reach the server from other origins
            std::string host = req.get_header_value("Host");
            if (host != std::format("127.0.0.1:{}", port) && host != std::format("localhost:{}", port)) return refuse(403, "Invalid Host header");
            if (req.has_header("Origin")) {
                std::string origin = req.get_header_value("Origin");
                if (!origin.starts_with("http://127.0.0.1") && !origin.starts_with("http://localhost")) return refuse(403, "Invalid Origin header");
            }
            // Other users of the machine can reach localhost as well. 403 instead of 401, so clients do not attempt an OAuth flow.
            if (!SecretEquals(req.get_header_value("Authorization"), "Bearer " + token))
                return refuse(403, "Missing or wrong bearer token, see the mcp command in CoVer's trace debugger");
            if (req.method != "POST") {
                res.status = 405;
                res.set_header("Allow", "POST");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        }

        void HandleRequest(httplib::Request const& req, httplib::Response& res) {
            Json::Value message;
            std::string errs;
            std::istringstream body(req.body);
            if (!Json::parseFromStream(Json::CharReaderBuilder(), body, &message, &errs))
                return SetJson(res, 400, RpcError(Json::Value(), -32700, "Parse error: " + errs));

            Json::Value responses = Json::arrayValue;
            bool deferred_call = false;
            if (message.isArray()) {
                for (Json::Value const& msg : message) HandleMessage(msg, responses, req, false, deferred_call);
            } else {
                HandleMessage(message, responses, req, true, deferred_call);
            }
            if (responses.empty()) { // Only notifications or responses
                res.status = 202;
                return;
            }
            // The next run of CoVer responds, this process re-executes itself without returning. Unless that fails and it exits, then
            // this server is stopped, and the call fails like those still waiting.
            while (deferred_call && !stopping) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            SetJson(res, 200, message.isArray() ? responses : responses[0]);
        }

        // deferrable: whether the response of a tool call may be deferred (see DeferResponse), deferred_call is set if it was
        void HandleMessage(Json::Value const& msg, Json::Value& responses, httplib::Request const& req, bool deferrable, bool& deferred_call) {
            if (!msg.isMember("method") || !msg.isMember("id")) return; // Notification, or a response to a request we never send
            Json::Value const& id = msg["id"];
            std::string method = msg["method"].asString();
            Json::Value const& params = msg["params"];
            Json::Value response;
            response["jsonrpc"] = "2.0";
            response["id"] = id;
            if (method == "initialize") {
                std::string version = params["protocolVersion"].asString();
                response["result"]["protocolVersion"] = PROTOCOL_VERSIONS.contains(version) ? version : DEFAULT_PROTOCOL_VERSION;
                response["result"]["capabilities"]["tools"]["listChanged"] = false;
                response["result"]["serverInfo"]["name"] = "CoVer";
                response["result"]["serverInfo"]["version"] = "1.0";
                response["result"]["instructions"] = INSTRUCTIONS;
            } else if (method == "ping") {
                response["result"] = Json::objectValue;
            } else if (method == "tools/list") {
                response["result"]["tools"] = Json::arrayValue;
                for (ToolSpec const& spec : specs) {
                    Json::Value tool;
                    tool["name"] = spec.name;
                    tool["description"] = spec.description;
                    tool["inputSchema"] = spec.schema;
                    response["result"]["tools"].append(tool);
                }
            } else if (method == "tools/call") {
                std::string name = params["name"].asString();
                bool known = std::ranges::any_of(specs, [&](ToolSpec const& spec) { return spec.name == name; });
                if (!known) return (void)responses.append(RpcError(id, -32602, "Unknown tool " + name));
                std::shared_ptr<Job> job = std::make_shared<Job>(name, params.isMember("arguments") ? params["arguments"] : Json::Value(Json::objectValue),
                                                                 deferrable ? std::optional<Json::Value>(id) : std::nullopt, req.local_port, req.remote_port);
                std::optional<ToolResult> result = RunOnUI(job);
                deferred_call = deferred_call || !result;
                return (void)responses.append(ToolCallResponse(id, result.value_or(ToolResult{RESTARTING, true, name})));
            } else {
                return (void)responses.append(RpcError(id, -32601, "Method not found: " + method));
            }
            responses.append(response);
        }

        // Empty if the response is deferred, see DeferResponse
        std::optional<ToolResult> RunOnUI(std::shared_ptr<Job> const& job) {
            std::future<std::optional<ToolResult>> result = job->result.get_future();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!accepting) return ToolResult{refusal, true, job->tool};
                queue.push_back(job);
            }
            wake();
            while (result.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready) {
                if (stopping) return ToolResult{RESTARTING, true, job->tool};
            }
            return result.get();
        }
};

std::expected<std::string, std::string> GenerateToken() {
    unsigned char bytes[32];
    for (std::size_t got = 0; got < sizeof(bytes);) {
        ssize_t n = getrandom(bytes + got, sizeof(bytes) - got, 0);
        if (n < 0 && errno != EINTR) return std::unexpected(std::format("Could not generate a token: {}", std::strerror(errno)));
        if (n > 0) got += n;
    }
    std::string token;
    for (unsigned char b : bytes) token += std::format("{:02x}", b);
    return token;
}

// Key of the token in the user configuration
std::string const TOKEN_KEY = "mcp_token";

// The token clients have to send. Generated on first use and kept in the user configuration, so clients only need to be set up once.
export std::expected<std::string, std::string> LoadOrCreateToken() {
    Json::Value saved = UserConfig::Get(TOKEN_KEY);
    if (saved.isString() && saved.asString().size() >= 32) return saved.asString();
    std::expected<std::string, std::string> token = GenerateToken();
    if (!token) return token;
    std::expected<void, std::string> written = UserConfig::Set(TOKEN_KEY, *token); // Also replaces a malformed token
    if (!written) return std::unexpected(written.error());
    return token;
}

export struct StartResult {
    std::unique_ptr<Server> server; // Null if it failed
    std::string message; // Status for the user
};

// Key of the port in the user configuration, DEFAULT_PORT if there is none
std::string const PORT_KEY = "mcp_port";

// Starts the server on the configured port, with the token from LoadOrCreateToken
export StartResult Start(std::vector<ToolSpec> specs, std::function<void()> wake) {
    std::expected<std::string, std::string> token = LoadOrCreateToken();
    if (!token) return {nullptr, "The MCP server could not be started: " + token.error()};
    Json::Value port_setting = UserConfig::Get(PORT_KEY);
    int port = port_setting.isNull() ? DEFAULT_PORT : port_setting.isInt() ? port_setting.asInt() : 0;
    if (port <= 0 || port > 65535) return {nullptr, "The MCP server could not be started: the " + PORT_KEY + " setting is not a valid port"};
    std::unique_ptr<Server> server = std::make_unique<Server>(port, *token, std::move(specs), std::move(wake));
    if (!server->Running()) return {nullptr, "The MCP server could not be started: " + server->Error()};
    std::string url = server->Url();
    return {std::move(server), "MCP server listening at " + url};
}

} // namespace MCPServer
