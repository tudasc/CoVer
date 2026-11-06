module;

#include <curl/curl.h>
#include <json/json.h>

export module LLMClient;
import std;
import TraceTools;
import UserConfig;

// Client for the Chat Completions API, which local inference servers (vLLM, Ollama, llama.cpp, ...) and many providers implement
namespace LLMClient {

using TraceTools::ToolSpec;
using UserConfig::JsonString;
using TraceTools::Trim;

export struct ToolCall {
    std::string id;
    std::string name;
    Json::Value input; // Null if the model sent malformed arguments
};

export struct ToolOutput {
    std::string id; // Of the tool call
    std::string name;
    std::string text;
    bool error = false;
};

export struct Message {
    enum struct Role { USER, ASSISTANT };
    Role role;
    std::vector<std::string> texts;
    std::vector<ToolCall> tool_calls; // Assistant only
    std::vector<ToolOutput> tool_outputs; // User only, answering the tool calls of the previous message
    // Assistant only: the model's reasoning, and the field the server returned it in. It is sent back in the same field,
    // the server's chat template decides whether the model sees it again. Empty field if the server returned none.
    std::string reasoning;
    std::string reasoning_field;
};

export struct Reply {
    enum struct Stop { DONE, TOOL_USE, MAX_TOKENS, REFUSAL };
    Message message;
    Stop stop = Stop::DONE;
    std::vector<std::string> notices; // Events worth showing to the user
    std::string refusal; // Reason given for a refusal, may be empty
    std::uint64_t input_tokens = 0; // Including cached tokens
    std::uint64_t cached_tokens = 0;
    std::uint64_t output_tokens = 0; // Including reasoning tokens
    std::uint64_t reasoning_tokens = 0; // Zero if the server does not report them
};

// Settings of the connection, kept in the user's configuration
export struct Config {
    std::string base_url; // Up to and including /v1
    std::string model;
    std::string api_key; // Empty if the server needs none
    std::string effort; // Reasoning effort, empty for the server's default
    std::uint64_t max_tokens = 0; // Maximum length of a response, 0 for the server's default
};

// Error message if the settings are incomplete, otherwise empty
export std::string Validate(Config const& config) {
    if (config.base_url.empty()) return "Enter the URL of the server's API, e.g. http://localhost:8000/v1";
    if (!config.base_url.starts_with("http://") && !config.base_url.starts_with("https://")) return "The URL must start with http:// or https://";
    if (config.model.empty()) return "Enter the name of the model, as the server knows it";
    return "";
}

// Settings from the fields of a settings form, or what is wrong with them
export std::expected<Config, std::string> MakeConfig(std::string const& base_url, std::string const& model, std::string const& api_key, std::string const& effort, std::string const& max_tokens) {
    Config config = {.base_url = Trim(base_url), .model = Trim(model), .api_key = Trim(api_key), .effort = Trim(effort)};
    while (config.base_url.ends_with('/')) config.base_url.pop_back();
    std::string tokens = Trim(max_tokens);
    if (!tokens.empty()) {
        char const* end = tokens.data() + tokens.size();
        std::from_chars_result parsed = std::from_chars(tokens.data(), end, config.max_tokens);
        if (parsed.ec != std::errc() || parsed.ptr != end || config.max_tokens == 0)
            return std::unexpected("The maximum response length must be a positive number of tokens, or empty for the server's default");
    }
    std::string error = Validate(config);
    if (!error.empty()) return std::unexpected(error);
    return config;
}

std::expected<Json::Value, std::string> ParseJson(std::string const& text) {
    Json::Value json;
    std::string errs;
    std::istringstream stream(text);
    if (!Json::parseFromStream(Json::CharReaderBuilder(), stream, &json, &errs)) return std::unexpected(errs);
    return json;
}

// Key of the settings in the user configuration
std::string const SETTINGS_KEY = "llm";

// The saved settings, which may be incomplete, or why there are none
export std::expected<Config, std::string> LoadConfig() {
    Json::Value json = UserConfig::Get(SETTINGS_KEY);
    if (json.isNull()) return std::unexpected("No LLM server is set up yet.");
    if (!json.isObject()) return std::unexpected("Could not read the LLM settings, they are no JSON object.");
    Config config;
    std::pair<char const*, std::string*> const fields[] = {{"base_url", &config.base_url}, {"model", &config.model}, {"api_key", &config.api_key}, {"effort", &config.effort}};
    for (auto const& [key, field] : fields) {
        Json::Value const& value = json[key];
        if (!value.isNull() && !value.isString()) return std::unexpected(std::format("Could not read the LLM settings: {} must be a string.", key));
        *field = value.asString();
    }
    Json::Value const& max_tokens = json["max_tokens"];
    if (!max_tokens.isNull() && !max_tokens.isUInt64()) return std::unexpected("Could not read the LLM settings: max_tokens must be a non-negative integer.");
    config.max_tokens = max_tokens.asUInt64();
    while (config.base_url.ends_with('/')) config.base_url.pop_back();
    return config;
}

export std::expected<void, std::string> SaveConfig(Config const& config) {
    Json::Value json;
    json["base_url"] = config.base_url;
    json["model"] = config.model;
    json["api_key"] = config.api_key;
    json["effort"] = config.effort;
    json["max_tokens"] = static_cast<Json::UInt64>(config.max_tokens);
    return UserConfig::Set(SETTINGS_KEY, json);
}

struct Transfer {
    std::string response;
    long retry_after = -1;
    std::atomic<bool> const* cancel;
};

std::size_t WriteCallback(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
    static_cast<Transfer*>(userdata)->response.append(data, size * nmemb);
    return size * nmemb;
}

std::size_t HeaderCallback(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
    std::string header(data, size * nmemb);
    std::string const name = "retry-after:";
    if (header.size() > name.size() && std::equal(name.begin(), name.end(), header.begin(), [](char a, char b) { return a == std::tolower(b); }))
        static_cast<Transfer*>(userdata)->retry_after = std::atol(header.c_str() + name.size());
    return size * nmemb;
}

int ProgressCallback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Transfer*>(userdata)->cancel->load() ? 1 : 0; // Non-zero aborts the transfer
}

// Most servers report errors as {"error": {"message": ...}}, some as {"error": "..."}
std::string ApiErrorMessage(long status, std::string const& response) {
    std::expected<Json::Value, std::string> json = ParseJson(response);
    if (json && json->isMember("error")) {
        Json::Value const& error = (*json)["error"];
        return std::format("LLM API error {}: {}", status, error.isString() ? error.asString() : error["message"].asString());
    }
    return std::format("LLM API error {}: {}", status, response.substr(0, 500));
}

// Sleeps for the given duration, returns false if cancelled in between
bool Wait(std::chrono::milliseconds duration, std::atomic<bool> const& cancel) {
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        if (cancel) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !cancel;
}

// Must be called on the main thread before any request
export void Init() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// POSTs the body as JSON, or GETs if it is null, and returns the parsed response. Retries rate limits, server errors and connection errors.
std::expected<Json::Value, std::string> Request(std::string const& url, std::vector<std::string> const& headers, Json::Value const* body, std::atomic<bool> const& cancel, int max_attempts) {
    std::string payload = body ? JsonString(*body) : "";
    std::string last_error;
    long retry_after = -1; // Delay requested by the server for the next attempt
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (attempt > 0 && !Wait(std::chrono::seconds(retry_after > 0 ? std::min(retry_after, 60L) : 1L << attempt), cancel)) break;
        CURL* curl = curl_easy_init();
        if (!curl) return std::unexpected("Could not initialize libcurl");
        Transfer transfer = {.cancel = &cancel};
        curl_slist* header_list = curl_slist_append(nullptr, "content-type: application/json");
        for (std::string const& header : headers) header_list = curl_slist_append(header_list, header.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        if (body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
        }
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // Required for use in threads
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 900L);
        CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);

        if (cancel) return std::unexpected("Request cancelled");
        if (rc != CURLE_OK) {
            last_error = std::format("Could not reach the LLM API at {}: {}", url, curl_easy_strerror(rc));
            retry_after = -1;
            continue;
        }
        if (status == 429 || status >= 500) {
            last_error = ApiErrorMessage(status, transfer.response);
            retry_after = transfer.retry_after;
            continue;
        }
        if (status != 200) return std::unexpected(ApiErrorMessage(status, transfer.response));
        std::expected<Json::Value, std::string> json = ParseJson(transfer.response);
        if (!json) return std::unexpected("Could not parse the LLM API response: " + json.error());
        return *json;
    }
    if (cancel) return std::unexpected("Request cancelled");
    return std::unexpected(last_error);
}

std::string Join(std::vector<std::string> const& texts) {
    std::string out;
    for (std::string const& text : texts) out += (out.empty() ? "" : "\n\n") + text;
    return out;
}

void EncodeMessage(Message const& msg, Json::Value& messages) {
    if (msg.role == Message::Role::USER) {
        for (ToolOutput const& output : msg.tool_outputs) {
            Json::Value tool;
            tool["role"] = "tool";
            tool["tool_call_id"] = output.id;
            tool["content"] = output.error ? "Error: " + output.text : output.text;
            messages.append(tool);
        }
        if (msg.texts.empty()) return;
        Json::Value user;
        user["role"] = "user";
        user["content"] = Join(msg.texts);
        messages.append(user);
        return;
    }
    Json::Value assistant;
    assistant["role"] = "assistant";
    assistant["content"] = msg.texts.empty() && !msg.tool_calls.empty() ? Json::Value() : Json::Value(Join(msg.texts));
    if (!msg.reasoning_field.empty()) assistant[msg.reasoning_field] = msg.reasoning;
    for (ToolCall const& call : msg.tool_calls) {
        Json::Value tool_call;
        tool_call["id"] = call.id;
        tool_call["type"] = "function";
        tool_call["function"]["name"] = call.name;
        tool_call["function"]["arguments"] = JsonString(call.input.isObject() ? call.input : Json::Value(Json::objectValue));
        assistant["tool_calls"].append(tool_call);
    }
    messages.append(assistant);
}

Json::Value ParseArguments(Json::Value const& arguments) {
    if (arguments.isObject()) return arguments; // Some servers send an object instead of a string
    if (!arguments.isString()) return Json::Value();
    std::string text = arguments.asString();
    if (text.empty()) return Json::Value(Json::objectValue);
    std::expected<Json::Value, std::string> json = ParseJson(text);
    if (!json || !json->isObject()) return Json::Value();
    return *json;
}

Reply DecodeReply(Json::Value const& response) {
    Reply reply;
    reply.message.role = Message::Role::ASSISTANT;
    Json::Value const& usage = response["usage"];
    reply.input_tokens = usage["prompt_tokens"].asUInt64();
    reply.cached_tokens = usage["prompt_tokens_details"]["cached_tokens"].asUInt64();
    reply.output_tokens = usage["completion_tokens"].asUInt64();
    reply.reasoning_tokens = usage["completion_tokens_details"]["reasoning_tokens"].asUInt64();
    Json::Value const& choice = response["choices"][0];
    Json::Value const& message = choice["message"];
    std::string finish_reason = choice["finish_reason"].asString();
    if (!message["refusal"].asString().empty()) { // Null unless refused
        reply.stop = Reply::Stop::REFUSAL;
        reply.refusal = message["refusal"].asString();
        return reply;
    }
    if (finish_reason == "content_filter") {
        reply.stop = Reply::Stop::REFUSAL;
        reply.refusal = "content filter";
        return reply;
    }
    // Not standardized: vLLM and Ollama use reasoning, llama.cpp and SGLang reasoning_content. Prefers a non-empty one if
    // both are sent. Empty reasoning is kept, as some templates distinguish it from missing reasoning.
    for (char const* field : {"reasoning", "reasoning_content"}) {
        if (!message[field].isString()) continue;
        if (!reply.message.reasoning_field.empty() && message[field].asString().empty()) continue;
        reply.message.reasoning_field = field;
        reply.message.reasoning = message[field].asString();
        if (!reply.message.reasoning.empty()) break;
    }
    if (!Trim(message["content"].asString()).empty()) reply.message.texts.push_back(message["content"].asString());
    bool cut_off = finish_reason == "length";
    for (Json::Value const& call : message["tool_calls"]) {
        std::string name = call["function"]["name"].asString();
        Json::Value input = ParseArguments(call["function"]["arguments"]);
        // The arguments of a call cut off at the maximum length are incomplete. Dropping it keeps the model from retrying it
        // because of a malformed arguments error, which would likely be cut off again.
        if (cut_off && input.isNull()) {
            reply.notices.push_back(std::format("Discarded the call of tool {}, it was cut off at the maximum length", name.empty() ? "?" : name));
            continue;
        }
        std::string id = call["id"].asString();
        if (id.empty()) id = std::format("call_{}", reply.message.tool_calls.size()); // Some servers omit ids
        reply.message.tool_calls.push_back({id, name, input});
    }
    if (cut_off) reply.stop = Reply::Stop::MAX_TOKENS; // Takes precedence over complete tool calls, so a cut off model is not continued automatically
    else if (!reply.message.tool_calls.empty()) reply.stop = Reply::Stop::TOOL_USE;
    return reply;
}

std::vector<std::string> AuthHeaders(Config const& config) {
    if (config.api_key.empty()) return {}; // Local servers may not need a key
    return {"authorization: Bearer " + config.api_key};
}

// Next assistant message for the conversation, blocking. Safe to call from any thread after Init().
export std::expected<Reply, std::string> Complete(Config const& config, std::string const& system, std::vector<ToolSpec> const& tools, std::vector<Message> const& messages, std::atomic<bool> const& cancel) {
    Json::Value body;
    body["model"] = config.model;
    if (!config.effort.empty()) body["reasoning_effort"] = config.effort;
    if (config.max_tokens > 0) body["max_tokens"] = static_cast<Json::UInt64>(config.max_tokens); // Not max_completion_tokens, which not all servers support
    body["messages"] = Json::arrayValue;
    Json::Value system_msg;
    system_msg["role"] = "system";
    system_msg["content"] = system;
    body["messages"].append(system_msg);
    for (Message const& msg : messages) EncodeMessage(msg, body["messages"]);
    body["tools"] = Json::arrayValue;
    for (ToolSpec const& tool : tools) {
        Json::Value def;
        def["type"] = "function";
        def["function"]["name"] = tool.name;
        def["function"]["description"] = tool.description;
        def["function"]["parameters"] = tool.schema;
        body["tools"].append(def);
    }

    std::expected<Json::Value, std::string> response = Request(config.base_url + "/chat/completions", AuthHeaders(config), &body, cancel, 4);
    if (!response) return std::unexpected(response.error());
    return DecodeReply(*response);
}

// Checks that the server is reachable, accepts the API key and offers the model, by fetching its model list.
// Blocking, safe to call from any thread after Init().
export std::expected<void, std::string> TestConnection(Config const& config, std::atomic<bool> const& cancel) {
    std::string invalid = Validate(config);
    if (!invalid.empty()) return std::unexpected(invalid);
    std::expected<Json::Value, std::string> response = Request(config.base_url + "/models", AuthHeaders(config), nullptr, cancel, 1);
    if (cancel) return std::unexpected("Cancelled");
    if (!response) return std::unexpected(response.error());
    if (!response->isObject() || !(*response)["data"].isArray())
        return std::unexpected("The server's answer is no model list. Check that the URL includes the API's base path, usually /v1.");
    std::string offered;
    for (Json::Value const& model : (*response)["data"]) {
        std::string id = model["id"].asString();
        if (id == config.model || id == config.model + ":latest") return {}; // Ollama adds the default tag
        offered += " " + id;
    }
    if (offered.empty()) return std::unexpected(std::format("The server offers no models, so {} is not available.", config.model));
    return std::unexpected(std::format("The server does not offer the model {}.\nModels offered by the server:{}", config.model, offered));
}

} // namespace LLMClient
