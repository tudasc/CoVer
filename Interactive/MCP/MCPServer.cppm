module;

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <set>
#include <string>
#include <sys/random.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <json/json.h>

export module MCPServer;
import TraceTools;
import UserConfig;

// Serves the trace tools to MCP clients (e.g. coding agents) over the Streamable HTTP transport, on localhost only.
// The server is stateless and answers with plain JSON. Tool calls are executed on the UI thread, see ProcessPending.
namespace MCPServer {

using TraceTools::ToolResult;
using TraceTools::ToolSpec;

export constexpr int DEFAULT_PORT = 7341;

// For tool calls while CoVer exits, it may restart for a reanalysis
export std::string const RESTARTING = "CoVer is restarting for a reanalysis (or was closed). If it restarts, wait a few seconds and then try again.";

std::string const INSTRUCTIONS = "Tools for CoVer's interactive mode, in which contract violations reported by CoVer's static analysis are inspected and false positives "
                                 "are fixed with annotations. Call get_debug_context first: it explains how to interpret traces and annotations, how to work through "
                                 "the reports and reanalyse, and the current state.";

std::set<std::string> const PROTOCOL_VERSIONS = {"2025-11-25", "2025-06-18", "2025-03-26"};
std::string const DEFAULT_PROTOCOL_VERSION = "2025-06-18";

constexpr size_t MAX_HEADER_SIZE = 64 * 1024;
constexpr size_t MAX_BODY_SIZE = 16 * 1024 * 1024;

export struct Activity {
    std::string summary;
    bool error;
};

struct Job {
    std::string tool;
    Json::Value args;
    std::promise<ToolResult> result;
};

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers; // Lower case names
    std::string body;
};

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string Trim(std::string const& s) {
    size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) return "";
    return s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
}

// Comparison taking the same time for all inputs of the same length
bool SecretEquals(std::string const& a, std::string const& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

bool SendAll(int fd, std::string const& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL); // A vanished client must not raise SIGPIPE
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

void SendResponse(int fd, int status, std::string const& reason, std::string const& body, std::string const& extra_headers = "") {
    SendAll(fd, std::format("HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n{}\r\n{}",
                            status, reason, body.size(), extra_headers, body));
}

std::string JsonString(Json::Value const& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["emitUTF8"] = true;
    return Json::writeString(writer, value);
}

Json::Value RpcError(Json::Value const& id, int code, std::string const& message) {
    Json::Value response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"]["code"] = code;
    response["error"]["message"] = message;
    return response;
}

// Reads one HTTP request, returns false on malformed or oversized requests
bool ReadRequest(int fd, Request& req) {
    std::string data;
    char buf[8192];
    size_t header_end;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        if (data.size() > MAX_HEADER_SIZE) return false;
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        data.append(buf, n);
    }
    std::string head = data.substr(0, header_end);
    std::string rest = data.substr(header_end + 4);
    size_t line_end = head.find("\r\n");
    std::string request_line = head.substr(0, line_end);
    size_t sp1 = request_line.find(' ');
    size_t sp2 = request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req.method = request_line.substr(0, sp1);
    req.path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    for (size_t pos = line_end; pos != std::string::npos && pos < head.size();) {
        size_t next = head.find("\r\n", pos + 2);
        std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
        size_t colon = line.find(':');
        if (colon != std::string::npos) req.headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
        pos = next;
    }

    auto readMore = [&]() {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        rest.append(buf, n);
        return rest.size() <= MAX_BODY_SIZE + MAX_HEADER_SIZE;
    };
    if (Lower(req.headers["transfer-encoding"]).find("chunked") != std::string::npos) {
        size_t pos = 0;
        while (true) {
            size_t size_end;
            while ((size_end = rest.find("\r\n", pos)) == std::string::npos) if (!readMore()) return false;
            size_t chunk = std::strtoul(rest.substr(pos, size_end - pos).c_str(), nullptr, 16);
            if (chunk == 0) return true;
            if (req.body.size() + chunk > MAX_BODY_SIZE) return false;
            while (rest.size() < size_end + 2 + chunk + 2) if (!readMore()) return false;
            req.body += rest.substr(size_end + 2, chunk);
            pos = size_end + 2 + chunk + 2;
        }
    }
    size_t length = std::strtoul(req.headers["content-length"].c_str(), nullptr, 10);
    if (length > MAX_BODY_SIZE) return false;
    while (rest.size() < length) if (!readMore()) return false;
    req.body = rest.substr(0, length);
    return true;
}

export class Server {
    public:
        // wake is called from the server thread whenever a tool call waits for ProcessPending
        Server(int port, std::string token, std::vector<ToolSpec> specs, std::function<void()> wake) : port{port}, token{std::move(token)}, specs{std::move(specs)}, wake{std::move(wake)} {
            if (pipe2(stop_pipe, O_CLOEXEC) != 0) {
                error = std::format("Could not create pipe: {}", std::strerror(errno));
                return;
            }
            listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            int one = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // Never reachable from other hosts
            if (listen_fd < 0 || bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_fd, 16) != 0) {
                error = std::format("Could not listen on 127.0.0.1:{}: {}", port, std::strerror(errno));
                return;
            }
            thread = std::thread([this] { Serve(); });
        }

        ~Server() {
            stopping = true;
            if (thread.joinable()) {
                char c = 0;
                [[maybe_unused]] ssize_t n = write(stop_pipe[1], &c, 1);
                thread.join();
            }
            FailPending(RESTARTING);
            for (int fd : {listen_fd, stop_pipe[0], stop_pipe[1]}) if (fd >= 0) close(fd);
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
                ToolResult result = run(job->tool, job->args);
                job->result.set_value(result);
                if (on_done) on_done({result.summary, result.error});
            }
        }

    private:
        int port;
        std::string token;
        std::vector<ToolSpec> specs;
        std::function<void()> wake;
        std::string error;
        int listen_fd = -1;
        int stop_pipe[2] = {-1, -1};
        std::thread thread;
        std::atomic<bool> stopping = false;
        std::mutex mutex; // Guards the members below
        bool accepting = false;
        std::string refusal = "CoVer is not accepting tool calls";
        std::vector<std::shared_ptr<Job>> queue;

        void FailPending(std::string const& message) {
            std::vector<std::shared_ptr<Job>> jobs;
            {
                std::lock_guard<std::mutex> lock(mutex);
                jobs.swap(queue);
            }
            for (std::shared_ptr<Job> const& job : jobs) job->result.set_value({message, true, job->tool});
        }

        void Serve() {
            while (!stopping) {
                pollfd fds[2] = {{listen_fd, POLLIN, 0}, {stop_pipe[0], POLLIN, 0}};
                if (poll(fds, 2, -1) < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (fds[1].revents) break;
                if (!(fds[0].revents & POLLIN)) continue;
                int fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd < 0) continue;
                timeval timeout = {.tv_sec = 10, .tv_usec = 0};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                HandleConnection(fd);
                close(fd);
            }
        }

        void HandleConnection(int fd) {
            Request req;
            if (!ReadRequest(fd, req)) return SendResponse(fd, 400, "Bad Request", JsonString(RpcError(Json::Value(), -32600, "Malformed HTTP request")));
            if (req.path.substr(0, req.path.find('?')) != "/mcp") return SendResponse(fd, 404, "Not Found", JsonString(RpcError(Json::Value(), -32600, "The MCP endpoint is /mcp")));
            // Protection against DNS rebinding, browsers must not be able to reach the server from other origins
            std::string host = req.headers["host"];
            if (host != std::format("127.0.0.1:{}", port) && host != std::format("localhost:{}", port))
                return SendResponse(fd, 403, "Forbidden", JsonString(RpcError(Json::Value(), -32600, "Invalid Host header")));
            if (req.headers.contains("origin")) {
                std::string origin = req.headers["origin"];
                if (!origin.starts_with("http://127.0.0.1") && !origin.starts_with("http://localhost"))
                    return SendResponse(fd, 403, "Forbidden", JsonString(RpcError(Json::Value(), -32600, "Invalid Origin header")));
            }
            // Other users of the machine can reach localhost as well. 403 instead of 401, so clients do not attempt an OAuth flow.
            if (!SecretEquals(req.headers["authorization"], "Bearer " + token))
                return SendResponse(fd, 403, "Forbidden", JsonString(RpcError(Json::Value(), -32600, "Missing or wrong bearer token, see the mcp command in CoVer's trace debugger")));
            if (req.method != "POST") return SendResponse(fd, 405, "Method Not Allowed", "", "Allow: POST\r\n");

            Json::Value message;
            Json::CharReaderBuilder builder;
            std::string errs;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if (!reader->parse(req.body.data(), req.body.data() + req.body.size(), &message, &errs))
                return SendResponse(fd, 400, "Bad Request", JsonString(RpcError(Json::Value(), -32700, "Parse error: " + errs)));

            Json::Value responses = Json::arrayValue;
            if (message.isArray()) {
                for (Json::Value const& msg : message) HandleMessage(msg, responses);
            } else {
                HandleMessage(message, responses);
            }
            if (responses.empty()) return SendResponse(fd, 202, "Accepted", ""); // Only notifications or responses
            SendResponse(fd, 200, "OK", JsonString(message.isArray() ? responses : responses[0]));
        }

        void HandleMessage(Json::Value const& msg, Json::Value& responses) {
            if (!msg.isObject()) return (void)responses.append(RpcError(Json::Value(), -32600, "Invalid request"));
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
                ToolResult result = RunOnUI(name, params.isMember("arguments") ? params["arguments"] : Json::Value(Json::objectValue));
                Json::Value content;
                content["type"] = "text";
                content["text"] = result.text;
                response["result"]["content"].append(content);
                response["result"]["isError"] = result.error;
            } else {
                return (void)responses.append(RpcError(id, -32601, "Method not found: " + method));
            }
            responses.append(response);
        }

        ToolResult RunOnUI(std::string const& tool, Json::Value const& args) {
            std::shared_ptr<Job> job = std::make_shared<Job>(tool, args);
            std::future<ToolResult> result = job->result.get_future();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!accepting) return {refusal, true, tool};
                queue.push_back(job);
            }
            wake();
            while (result.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready) {
                if (stopping) return {RESTARTING, true, tool};
            }
            return result.get();
        }
};

std::expected<std::string, std::string> GenerateToken() {
    unsigned char bytes[32];
    for (size_t got = 0; got < sizeof(bytes);) {
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
