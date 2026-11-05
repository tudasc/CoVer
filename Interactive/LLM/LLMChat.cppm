module;

#include <cerrno>

#include <json/json.h>

export module LLMChat;
import std;
import AnalysisReports;
import UserConfig;
import InteractiveSession;
import InteractiveUtils;
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

std::string const CHAT_PROMPT = R"(You are an assistant built into CoVer's trace debugger. The user chats with you in a small window, possibly in a terminal: be concise, prefer short paragraphs and lists, avoid tables and headings. Annotations you add are shown in the trace once the user closes the chat. You cannot reanalyse, the user does that after closing the chat. The conversation continues when the user opens another trace or reanalyses, you are told when that happens.)";

// The conversation of the chat. It lasts for the whole interactive session: it continues when another trace is opened,
// and is carried over to reanalyses through CHAT_CACHE_FILE.
struct Conversation {
    std::vector<Message> messages;
    std::vector<ChatEntry> transcript;
    std::uint64_t input_tokens = 0, cached_tokens = 0, output_tokens = 0, reasoning_tokens = 0;
    std::string model_trace; // Key of the trace open during the model's last turn, see InteractiveUtils::TraceKey
    std::string user_trace; // Key of the trace the chat was last opened in
    bool reanalysed = false; // Since the model's last turn
    bool announce_reanalysis = false; // The user has not been told yet that the chat continues after the reanalysis
};

Json::Value ToJson(Message const& message) {
    Json::Value json;
    json["role"] = message.role == Message::Role::USER ? "user" : "assistant";
    json["texts"] = Json::arrayValue;
    for (std::string const& text : message.texts) json["texts"].append(text);
    json["tool_calls"] = Json::arrayValue;
    for (LLMClient::ToolCall const& call : message.tool_calls) {
        Json::Value entry;
        entry["id"] = call.id;
        entry["name"] = call.name;
        entry["input"] = call.input;
        json["tool_calls"].append(entry);
    }
    json["tool_outputs"] = Json::arrayValue;
    for (LLMClient::ToolOutput const& output : message.tool_outputs) {
        Json::Value entry;
        entry["id"] = output.id;
        entry["name"] = output.name;
        entry["text"] = output.text;
        entry["error"] = output.error;
        json["tool_outputs"].append(entry);
    }
    json["reasoning"] = message.reasoning;
    json["reasoning_field"] = message.reasoning_field;
    return json;
}

Message MessageFromJson(Json::Value const& json) {
    Message message = {json["role"].asString() == "user" ? Message::Role::USER : Message::Role::ASSISTANT};
    for (Json::Value const& text : json["texts"]) message.texts.push_back(text.asString());
    for (Json::Value const& call : json["tool_calls"]) message.tool_calls.push_back({call["id"].asString(), call["name"].asString(), call["input"]});
    for (Json::Value const& output : json["tool_outputs"])
        message.tool_outputs.push_back({output["id"].asString(), output["name"].asString(), output["text"].asString(), output["error"].asBool()});
    message.reasoning = json["reasoning"].asString();
    message.reasoning_field = json["reasoning_field"].asString();
    return message;
}

// The conversation saved before the reanalysis, empty if there is none
Conversation LoadConversation() {
    std::ifstream file(InteractiveUtils::CHAT_CACHE_FILE);
    if (!file) return {};
    Json::Value json;
    file >> json;
    Conversation conv;
    for (Json::Value const& message : json["messages"]) conv.messages.push_back(MessageFromJson(message));
    for (Json::Value const& entry : json["transcript"])
        conv.transcript.push_back({static_cast<ChatEntry::Kind>(entry["kind"].asInt()), entry["text"].asString(), entry["detail"].asString()});
    conv.input_tokens = json["input_tokens"].asUInt64();
    conv.cached_tokens = json["cached_tokens"].asUInt64();
    conv.output_tokens = json["output_tokens"].asUInt64();
    conv.reasoning_tokens = json["reasoning_tokens"].asUInt64();
    conv.model_trace = json["model_trace"].asString();
    conv.user_trace = json["user_trace"].asString();
    conv.reanalysed = conv.announce_reanalysis = true;
    return conv;
}

std::optional<Conversation> conversation;

// The conversation of this interactive session, restored from before the reanalysis on first use
Conversation& Current() {
    if (!conversation) conversation = LoadConversation();
    return *conversation;
}

// Whether there is a chat log to export: the chat was used in this interactive session, possibly before a reanalysis
export bool HasConversation() {
    return !Current().transcript.empty();
}

// Writes the conversation as JSON: the messages sent to the model, the transcript shown to the user (with the full tool
// outputs) and the token usage. Used for exporting the chat log and keeping it for the reanalysis. Returns why it failed, empty on success.
export std::string ExportConversation(std::string const& path) {
    Conversation const& conv = Current();
    Json::Value json;
    json["messages"] = Json::arrayValue;
    for (Message const& message : conv.messages) json["messages"].append(ToJson(message));
    json["transcript"] = Json::arrayValue;
    for (ChatEntry const& entry : conv.transcript) {
        Json::Value item;
        item["kind"] = std::to_underlying(entry.kind);
        item["text"] = entry.text;
        item["detail"] = entry.detail;
        json["transcript"].append(item);
    }
    json["input_tokens"] = static_cast<Json::UInt64>(conv.input_tokens);
    json["cached_tokens"] = static_cast<Json::UInt64>(conv.cached_tokens);
    json["output_tokens"] = static_cast<Json::UInt64>(conv.output_tokens);
    json["reasoning_tokens"] = static_cast<Json::UInt64>(conv.reasoning_tokens);
    json["model_trace"] = conv.model_trace;
    json["user_trace"] = conv.user_trace;
    std::ofstream file(path);
    if (!file) return std::format("Could not open {} for writing: {}", path, std::strerror(errno));
    file << UserConfig::JsonString(json) << "\n";
    file.close();
    return file ? "" : std::format("Could not write {}", path);
}

// Default file name for exporting the chat log
export std::string const EXPORT_FILE = "CoVer_ChatLog.json";

export class ChatSession {
    public:
        ChatSession(TraceSession& session, InteractiveUtils::PostFn post) : session{session}, tools{session}, post{std::move(post)}, conv{Current()} {
            LLMClient::Init();
            // Fixed while the trace is open, so servers can cache it
            system_prompt = CHAT_PROMPT + "\n\n" + TraceTools::GUIDANCE + "\n\n" + tools.Context();
            for (ToolSpec const& spec : TraceTools::Specs()) {
                if (spec.name != TraceTools::CONTEXT_TOOL) tool_specs.push_back(spec); // Already part of the system prompt
            }
            seen_history = session.GetHistory().size();
            std::string key = InteractiveUtils::TraceKey(session.Context());
            if (!conv.transcript.empty() && (conv.announce_reanalysis || conv.user_trace != key)) {
                AnalysisReports::DebugContext const& ctx = session.Context();
                conv.transcript.push_back({ChatEntry::Kind::INFO, std::format("{} The chat continues on the trace of {}: {}.",
                                                                              conv.announce_reanalysis ? "Reanalysed with the annotations added so far." : "Opened another trace.", ctx.function, ctx.formula)});
            }
            conv.user_trace = key;
            conv.announce_reanalysis = false;
        }

        ~ChatSession() {
            if (cancel) *cancel = true;
        }

        ChatSession(ChatSession const&) = delete;
        ChatSession& operator=(ChatSession const&) = delete;

        // Settings for the following requests, set once the connection works
        void SetConfig(LLMClient::Config const& new_config) { config = new_config; }

        std::vector<ChatEntry> const& Transcript() const { return conv.transcript; }
        bool Busy() const { return busy; }
        std::string Status() const {
            if (!config) return "Not connected";
            std::string usage = std::format("{} input tokens ({} cached), {} output tokens", conv.input_tokens, conv.cached_tokens, conv.output_tokens);
            if (conv.reasoning_tokens > 0) usage += std::format(" ({} reasoning)", conv.reasoning_tokens);
            if (busy) return std::format("Waiting for {} ({}s) | {}", config->model, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - request_start).count(), usage);
            return std::format("{} | {}", config->model, usage);
        }

        // Called on the UI thread whenever the transcript or busy state changes. Set to nullptr when the window closes.
        void SetListener(std::function<void()> listener) { on_change = std::move(listener); }

        void Send(std::string const& text) {
            if (busy || text.empty()) return;
            conv.transcript.push_back({ChatEntry::Kind::USER, text});
            if (!config) {
                conv.transcript.push_back({ChatEntry::Kind::ERROR, "Not connected to an LLM server"});
                return Notify();
            }
            std::string content = text;
            std::string user_cmds = NewUserCommands();
            if (!user_cmds.empty()) content = std::format("[Trace debugger commands the user ran since your last turn:\n{}]\n\n{}", user_cmds, content);
            std::string key = InteractiveUtils::TraceKey(session.Context());
            if (!conv.messages.empty() && (conv.reanalysed || conv.model_trace != key)) content = std::format("[{}]\n\n{}", TraceChangeNote(key), content);
            conv.model_trace = key;
            conv.reanalysed = false;
            // After a cancelled or failed turn the conversation already ends with a user message, which is continued
            if (conv.messages.empty() || conv.messages.back().role != Message::Role::USER) conv.messages.push_back({Message::Role::USER});
            conv.messages.back().texts.push_back(content);
            tool_rounds = 0;
            Request();
        }

        void Cancel() {
            if (!busy) return;
            *cancel = true;
            busy = false;
            generation++;
            conv.transcript.push_back({ChatEntry::Kind::INFO, "Cancelled. Annotations added so far are kept."});
            Notify();
        }

    private:
        TraceSession& session;
        TraceTools::TraceTools tools;
        InteractiveUtils::PostFn post;
        std::optional<LLMClient::Config> config;
        std::string system_prompt;
        std::vector<ToolSpec> tool_specs;
        Conversation& conv;
        std::function<void()> on_change;
        bool busy = false;
        std::uint64_t generation = 0; // Identifies the pending request, responses to older ones are dropped
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<bool> alive = std::make_shared<bool>(true); // Expires with the session, guards posted tasks
        int tool_rounds = 0;
        std::chrono::steady_clock::time_point request_start;
        std::size_t seen_history = 0; // History entries the model knows about

        // Tells the model that the trace changed since its last turn
        std::string TraceChangeNote(std::string const& key) const {
            std::string what = conv.reanalysed ? std::format("CoVer reanalysed the program with the annotations added so far, and the user opened {}", key == conv.model_trace ? "the trace of the same formula again" : "another trace")
                                               : "The user opened another trace";
            return what + ". It is described in the system prompt. Block and instruction numbers in earlier messages refer to the previous trace and may no longer apply, call get_trace before relying on them.";
        }

        void Notify() {
            if (on_change) on_change();
        }

        // Commands the user ran in the debugger since the model's last turn, so it knows about manual changes
        std::string NewUserCommands() {
            InteractiveSession::History const& history = session.GetHistory();
            std::string cmds;
            for (std::size_t i = seen_history; i < history.size(); i++) {
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
            std::uint64_t id = ++generation;
            cancel = std::make_shared<std::atomic<bool>>(false);
            std::weak_ptr<bool> guard = alive;
            // The worker only uses copies, the session may be gone when it finishes
            std::thread([this, guard, id, config = *config, system = system_prompt, specs = tool_specs, msgs = conv.messages, flag = cancel, post = post] {
                std::expected<Reply, std::string> res = LLMClient::Complete(config, system, specs, msgs, *flag);
                post([this, guard, id, res = std::move(res)] {
                    if (guard.lock()) HandleReply(id, res);
                });
            }).detach();
            Notify();
        }

        void HandleReply(std::uint64_t id, std::expected<Reply, std::string> const& res) {
            if (id != generation || !busy) return; // Cancelled
            busy = false;
            if (!res) {
                conv.transcript.push_back({ChatEntry::Kind::ERROR, res.error()});
                return Notify();
            }
            Reply const& reply = *res;
            conv.input_tokens += reply.input_tokens;
            conv.cached_tokens += reply.cached_tokens;
            conv.output_tokens += reply.output_tokens;
            conv.reasoning_tokens += reply.reasoning_tokens;
            // Shown even without a message, the response may have been cut off while reasoning
            if (!reply.message.reasoning.empty()) conv.transcript.push_back({ChatEntry::Kind::THINKING, reply.message.reasoning});
            for (std::string const& notice : reply.notices) conv.transcript.push_back({ChatEntry::Kind::INFO, notice});
            if (reply.stop == Reply::Stop::REFUSAL) {
                conv.transcript.push_back({ChatEntry::Kind::ERROR, "The model declined to respond" + (reply.refusal.empty() ? "" : " (" + reply.refusal + ")") + ". Try rephrasing your message."});
                return Notify();
            }
            if (reply.message.texts.empty() && reply.message.tool_calls.empty()) {
                if (reply.stop == Reply::Stop::MAX_TOKENS) conv.transcript.push_back({ChatEntry::Kind::INFO, CUT_OFF_INFO});
                else conv.transcript.push_back({ChatEntry::Kind::ERROR, "The model sent an empty response."});
                return Notify();
            }
            conv.messages.push_back(reply.message);
            for (std::string const& text : reply.message.texts) conv.transcript.push_back({ChatEntry::Kind::ASSISTANT, text});

            Message results = {Message::Role::USER};
            for (LLMClient::ToolCall const& call : reply.message.tool_calls) {
                ToolResult result = call.input.isNull() ? ToolResult{"Malformed tool arguments, they must be a JSON object", true, call.name + "  ->  malformed arguments"}
                                                        : tools.Run(call.name, call.input);
                conv.transcript.push_back({result.error ? ChatEntry::Kind::ERROR : ChatEntry::Kind::TOOL, result.summary, result.text});
                results.tool_outputs.push_back({call.id, call.name, result.text, result.error});
            }
            seen_history = session.GetHistory().size(); // Commands of the model are no user commands

            bool used_tools = !results.tool_outputs.empty();
            if (used_tools) conv.messages.push_back(std::move(results));
            if (reply.stop == Reply::Stop::MAX_TOKENS) {
                conv.transcript.push_back({ChatEntry::Kind::INFO, CUT_OFF_INFO});
                return Notify();
            }
            if (!used_tools) return Notify();
            if (++tool_rounds >= MAX_TOOL_ROUNDS) {
                conv.transcript.push_back({ChatEntry::Kind::INFO, std::format("Paused after {} rounds of tool use. Send a message to let it continue.", MAX_TOOL_ROUNDS)});
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

// Texts of the chat, shown by both frontends
export std::string const CHAT_INTRO = "Ask the LLM about this trace, e.g. why the error is reported or whether it is a false positive. "
                                      "It can inspect the trace, the IR and the source code, and add annotations.";
export std::string const CHAT_ANNOTATIONS_HINT = "Annotations added by the LLM are recorded in the command history and shown in the trace once the chat is closed. "
                                                 "Reanalyse afterwards to check their effect.";

// A field of the settings form with its value
export struct SettingsField {
    std::string label;
    std::string placeholder;
    bool password = false;
    std::string value;
};

export std::string const SETTINGS_INTRO = "The chat works with servers implementing the OpenAI Chat Completions API, e.g. vLLM, llama.cpp, Ollama or SGLang. "
                                          "The model must support tool use. The settings are saved in your CoVer user settings, which only you can read.";

// Indices of the fields in SettingsForm, in the order the form shows them
export enum SettingsFieldIndex : std::size_t { FIELD_URL, FIELD_MODEL, FIELD_API_KEY, FIELD_EFFORT, FIELD_MAX_TOKENS, NUM_SETTINGS_FIELDS };

export using SettingsForm = std::array<SettingsField, NUM_SETTINGS_FIELDS>;

// The settings form, filled with the settings
export SettingsForm MakeSettingsForm(LLMClient::Config const& config) {
    SettingsForm form;
    form[FIELD_URL] = {.label = "Server URL", .placeholder = "e.g. http://localhost:8000/v1 (vLLM), http://localhost:11434/v1 (Ollama), http://localhost:8080/v1 (llama.cpp)", .value = config.base_url};
    form[FIELD_MODEL] = {.label = "Model", .placeholder = "Name of the model, as the server knows it", .value = config.model};
    form[FIELD_API_KEY] = {.label = "API key", .placeholder = "Only if the server requires one", .password = true, .value = config.api_key};
    form[FIELD_EFFORT] = {.label = "Reasoning effort", .placeholder = "Server default, e.g. low, medium, high", .value = config.effort};
    form[FIELD_MAX_TOKENS] = {.label = "Max response tokens", .placeholder = "Server default", .value = config.max_tokens > 0 ? std::to_string(config.max_tokens) : ""};
    return form;
}

// Settings from the values of the settings form, or what is wrong with them
export std::expected<LLMClient::Config, std::string> ConfigFromForm(SettingsForm const& form) {
    return LLMClient::MakeConfig(form[FIELD_URL].value, form[FIELD_MODEL].value, form[FIELD_API_KEY].value, form[FIELD_EFFORT].value, form[FIELD_MAX_TOKENS].value);
}

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

// The chat command of a trace debugger: shows the chat with open, creating it on first use. post runs tasks on the
// frontend's UI thread. The session lasts as long as the returned function and its copies, i.e. while the trace is open, the conversation
// lasts for the whole interactive session (see Conversation).
export std::function<void()> ChatCommand(TraceSession& session, InteractiveUtils::PostFn post, std::function<void(ChatSession&)> open) {
    std::shared_ptr<std::optional<ChatSession>> chat = std::make_shared<std::optional<ChatSession>>();
    return [&session, post = std::move(post), open = std::move(open), chat] {
        if (!*chat) chat->emplace(session, post);
        open(**chat);
    };
}

// Lets the user edit the saved settings, returns the outcome. Open chats pick them up when opened again.
export std::string Configure(ChatUI const& ui) {
    auto [config, error] = Load();
    if (!EditUntilConnected(config, error, ui)) return "Closed the LLM settings";
    return std::format("Saved the LLM settings, {} at {} is available", config.model, config.base_url);
}

} // namespace LLMChat
