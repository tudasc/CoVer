module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <json/json.h>

export module LLMChat;
import InteractiveSession;
import InteractiveCmds;
import TraceTools;
import LLMClient;

// Provider independent LLM chat for the trace debugger. The model works on the trace through the trace tools.
namespace LLMChat {

using InteractiveSession::TraceSession;
using LLMClient::Message;
using LLMClient::Reply;
using TraceTools::ToolResult;
using TraceTools::ToolSpec;

export struct ChatEntry {
    enum struct Kind { USER, ASSISTANT, THINKING, TOOL, INFO, ERROR };
    Kind kind;
    std::string text;
    std::string detail; // Tool output, shown on demand
};

constexpr int MAX_TOOL_ROUNDS = 40; // Per user message, to bound cost if the model gets stuck
std::string const CUT_OFF_INFO = "The response was cut off at the maximum length. Send a message to let it continue.";

std::string const CHAT_PROMPT = R"(You are an assistant built into CoVer's trace debugger. The user chats with you in a small window, possibly in a terminal: be concise, prefer short paragraphs and lists, avoid tables and headings. Annotations you add are shown in the trace once the user closes the chat. You cannot reanalyse, the user does that after closing the chat.)";

export class ChatSession {
    public:
        using PostFn = std::function<void(std::function<void()>)>;

        // post runs a task on the UI thread, it is called from a worker thread and must stay valid for the program's lifetime
        ChatSession(TraceSession& session, PostFn post) : session{session}, tools{session}, post{std::move(post)} {
            LLMClient::Init();
            // Fixed for the whole conversation, so servers can cache it
            system_prompt = CHAT_PROMPT + "\n\n" + TraceTools::GUIDANCE + "\n\n" + tools.Context();
            for (ToolSpec const& spec : TraceTools::Specs()) {
                if (spec.name != TraceTools::CONTEXT_TOOL) tool_specs.push_back(spec); // Already part of the system prompt
            }
            seen_history = session.GetHistory().size();
        }

        ~ChatSession() {
            if (cancel) *cancel = true;
        }

        ChatSession(ChatSession const&) = delete;
        ChatSession& operator=(ChatSession const&) = delete;

        // Settings for the following requests, set once the connection works
        void SetConfig(LLMClient::Config const& new_config) { config = new_config; }

        std::vector<ChatEntry> const& Transcript() const { return transcript; }
        bool Busy() const { return busy; }
        std::string Status() const {
            if (!config) return "Not connected";
            std::string usage = std::format("{} input tokens ({} cached), {} output tokens", input_tokens, cached_tokens, output_tokens);
            if (reasoning_tokens > 0) usage += std::format(" ({} reasoning)", reasoning_tokens);
            if (busy) return std::format("Waiting for {} ({}s) | {}", config->model, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - request_start).count(), usage);
            return std::format("{} | {}", config->model, usage);
        }

        // Called on the UI thread whenever the transcript or busy state changes. Set to nullptr when the window closes.
        void SetListener(std::function<void()> listener) { on_change = std::move(listener); }

        void Send(std::string const& text) {
            if (busy || text.empty()) return;
            transcript.push_back({ChatEntry::Kind::USER, text});
            if (!config) {
                transcript.push_back({ChatEntry::Kind::ERROR, "Not connected to an LLM server"});
                return Notify();
            }
            std::string content = text;
            std::string user_cmds = NewUserCommands();
            if (!user_cmds.empty()) content = std::format("[Trace debugger commands the user ran since your last turn:\n{}]\n\n{}", user_cmds, text);
            // After a cancelled or failed turn the conversation already ends with a user message, which is continued
            if (messages.empty() || messages.back().role != Message::Role::USER) messages.push_back({Message::Role::USER});
            messages.back().texts.push_back(content);
            tool_rounds = 0;
            Request();
        }

        void Cancel() {
            if (!busy) return;
            *cancel = true;
            busy = false;
            generation++;
            transcript.push_back({ChatEntry::Kind::INFO, "Cancelled. Annotations added so far are kept."});
            Notify();
        }

    private:
        TraceSession& session;
        TraceTools::TraceTools tools;
        PostFn post;
        std::optional<LLMClient::Config> config;
        std::string system_prompt;
        std::vector<ToolSpec> tool_specs;
        std::vector<Message> messages;
        std::vector<ChatEntry> transcript;
        std::function<void()> on_change;
        bool busy = false;
        uint64_t generation = 0; // Identifies the pending request, responses to older ones are dropped
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<bool> alive = std::make_shared<bool>(true); // Expires with the session, guards posted tasks
        int tool_rounds = 0;
        std::chrono::steady_clock::time_point request_start;
        size_t seen_history = 0; // History entries the model knows about
        uint64_t input_tokens = 0, cached_tokens = 0, output_tokens = 0, reasoning_tokens = 0;

        void Notify() {
            if (on_change) on_change();
        }

        // Commands the user ran in the debugger since the model's last turn, so it knows about manual changes
        std::string NewUserCommands() {
            InteractiveSession::History const& history = session.GetHistory();
            std::string cmds;
            for (size_t i = seen_history; i < history.size(); i++) {
                InteractiveCmds::CmdInfo const* cmd = InteractiveCmds::FindCommand(history[i].first);
                if (cmd && cmd->access == InteractiveCmds::CmdAccess::FRONTEND) continue;
                cmds += std::format("{} -> {}\n", history[i].first, history[i].second.res.empty() ? InteractiveSession::resultCodeStr(history[i].second.code) : TraceTools::FirstLine(history[i].second.res));
            }
            seen_history = history.size();
            return cmds;
        }

        void Request() {
            busy = true;
            request_start = std::chrono::steady_clock::now();
            uint64_t id = ++generation;
            cancel = std::make_shared<std::atomic<bool>>(false);
            std::weak_ptr<bool> guard = alive;
            // The worker only uses copies, the session may be gone when it finishes
            std::thread([this, guard, id, config = *config, system = system_prompt, specs = tool_specs, msgs = messages, flag = cancel, post = post] {
                std::expected<Reply, std::string> res = LLMClient::Complete(config, system, specs, msgs, *flag);
                post([this, guard, id, res = std::move(res)] {
                    if (guard.lock()) HandleReply(id, res);
                });
            }).detach();
            Notify();
        }

        void HandleReply(uint64_t id, std::expected<Reply, std::string> const& res) {
            if (id != generation || !busy) return; // Cancelled
            busy = false;
            if (!res) {
                transcript.push_back({ChatEntry::Kind::ERROR, res.error()});
                return Notify();
            }
            Reply const& reply = *res;
            input_tokens += reply.input_tokens;
            cached_tokens += reply.cached_tokens;
            output_tokens += reply.output_tokens;
            reasoning_tokens += reply.reasoning_tokens;
            // Shown even without a message, the response may have been cut off while reasoning
            if (!reply.message.reasoning.empty()) transcript.push_back({ChatEntry::Kind::THINKING, reply.message.reasoning});
            for (std::string const& notice : reply.notices) transcript.push_back({ChatEntry::Kind::INFO, notice});
            if (reply.stop == Reply::Stop::REFUSAL) {
                transcript.push_back({ChatEntry::Kind::ERROR, "The model declined to respond" + (reply.refusal.empty() ? "" : " (" + reply.refusal + ")") + ". Try rephrasing your message."});
                return Notify();
            }
            if (reply.message.texts.empty() && reply.message.tool_calls.empty()) {
                if (reply.stop == Reply::Stop::MAX_TOKENS) transcript.push_back({ChatEntry::Kind::INFO, CUT_OFF_INFO});
                else transcript.push_back({ChatEntry::Kind::ERROR, "The model sent an empty response."});
                return Notify();
            }
            messages.push_back(reply.message);
            for (std::string const& text : reply.message.texts) transcript.push_back({ChatEntry::Kind::ASSISTANT, text});

            Message results = {Message::Role::USER};
            for (LLMClient::ToolCall const& call : reply.message.tool_calls) {
                ToolResult result = call.input.isNull() ? ToolResult{"Malformed tool arguments, they must be a JSON object", true, call.name + "  ->  malformed arguments"}
                                                        : tools.Run(call.name, call.input);
                transcript.push_back({result.error ? ChatEntry::Kind::ERROR : ChatEntry::Kind::TOOL, result.summary, result.text});
                results.tool_outputs.push_back({call.id, call.name, result.text, result.error});
            }
            seen_history = session.GetHistory().size(); // Commands of the model are no user commands

            bool used_tools = !results.tool_outputs.empty();
            if (used_tools) messages.push_back(std::move(results));
            if (reply.stop == Reply::Stop::MAX_TOKENS) {
                transcript.push_back({ChatEntry::Kind::INFO, CUT_OFF_INFO});
                return Notify();
            }
            if (!used_tools) return Notify();
            if (++tool_rounds >= MAX_TOOL_ROUNDS) {
                transcript.push_back({ChatEntry::Kind::INFO, std::format("Paused after {} rounds of tool use. Send a message to let it continue.", MAX_TOOL_ROUNDS)});
                return Notify();
            }
            Request();
        }
};

// Connection test running on a worker thread, polled by the frontend
export class ConnectionTest {
    public:
        explicit ConnectionTest(LLMClient::Config const& config) : state{std::make_shared<State>()} {
            LLMClient::Init();
            // The worker only uses copies and the shared state, the test object may be gone when it finishes
            std::thread([state = state, config] {
                std::expected<void, std::string> result = LLMClient::TestConnection(config, state->cancel);
                std::lock_guard<std::mutex> lock(state->mutex);
                state->result = std::move(result);
            }).detach();
        }

        ~ConnectionTest() { state->cancel = true; }

        ConnectionTest(ConnectionTest const&) = delete;
        ConnectionTest& operator=(ConnectionTest const&) = delete;

        // Empty while the test runs
        std::optional<std::expected<void, std::string>> Result() const {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->result;
        }

    private:
        struct State {
            std::atomic<bool> cancel = false;
            std::mutex mutex;
            std::optional<std::expected<void, std::string>> result;
        };
        std::shared_ptr<State> state;
};

// The screens of the chat and its settings, which differ between the frontends
export struct ChatUI {
    // Lets the user edit the settings. The error tells why they are needed, it is empty if the user asked for them. False if cancelled.
    std::function<bool(LLMClient::Config& config, std::string const& error)> settings;
    // Shows that the connection is tested until the test is done, false if the user cancelled it
    std::function<bool(ConnectionTest const& test, LLMClient::Config const& config)> testing;
    // Shows the chat until it is closed
    std::function<void()> chat;
};

// Why the connection does not work, empty if it does
std::string Test(LLMClient::Config const& config, ChatUI const& ui) {
    ConnectionTest test(config);
    if (!ui.testing(test, config)) return "The connection test was cancelled.";
    std::expected<void, std::string> result = *test.Result();
    return result ? "" : "The connection test failed: " + result.error();
}

// Lets the user edit the settings until they are saved and the connection works. False if cancelled.
bool EditUntilConnected(LLMClient::Config& config, std::string error, ChatUI const& ui) {
    while (true) {
        LLMClient::Config edited = config;
        if (!ui.settings(edited, error)) return false;
        config = edited; // Shown again if saving or the test fails
        std::expected<void, std::string> saved = LLMClient::SaveConfig(config);
        error = saved ? Test(config, ui) : saved.error();
        if (error.empty()) return true;
    }
}

// Settings saved before, which may be incomplete, and why they cannot be used as they are
std::pair<LLMClient::Config, std::string> Load() {
    std::expected<LLMClient::Config, std::string> loaded = LLMClient::LoadConfig();
    LLMClient::Config config = loaded.value_or(LLMClient::Config());
    return {config, loaded ? LLMClient::Validate(config) : loaded.error()};
}

// Tests the connection with the saved settings, then shows the chat. If there are no settings or the test fails,
// the user can edit the settings, which are saved before testing them again.
export void Open(ChatSession& chat, ChatUI const& ui) {
    auto [config, error] = Load();
    if (error.empty()) error = Test(config, ui);
    if (!error.empty() && !EditUntilConnected(config, error, ui)) return;
    chat.SetConfig(config);
    ui.chat();
}

// Lets the user edit the saved settings, returns the outcome. Open chats pick them up when opened again.
export std::string Configure(ChatUI const& ui) {
    auto [config, error] = Load();
    if (!EditUntilConnected(config, error, ui)) return "Closed the LLM settings";
    return std::format("Saved the LLM settings, {} at {} is available", config.model, config.base_url);
}

} // namespace LLMChat
