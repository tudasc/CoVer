module;

#include <string>

export module TUISettings;
import InteractiveSession;
import TUIChat;

// The settings pages of the terminal UI, see InteractiveCmds::SETTINGS_PAGES
namespace TUISettings {

using InteractiveSession::CmdResult;
using InteractiveSession::CmdResultCode;

// Opens a settings page, returns the outcome
export CmdResult OpenSettings(std::string const& page) {
    if (page == "llm") return {CmdResultCode::SUCCESS_CONTINUE, TUIChat::OpenSettings()};
    return {CmdResultCode::INVALID_INPUT, "Unknown settings page " + page};
}

} // namespace TUISettings
