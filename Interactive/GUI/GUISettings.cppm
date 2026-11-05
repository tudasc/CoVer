export module GUISettings;
import std;
import InteractiveSession;
import GUIChat;

// The settings pages of the graphical UI, see InteractiveCmds::SETTINGS_PAGES
namespace GUISettings {

using InteractiveSession::CmdResult;
using InteractiveSession::CmdResultCode;

// Opens a settings page, returns the outcome
export CmdResult OpenSettings(std::string const& page) {
    if (page == "llm") return {CmdResultCode::SUCCESS_CONTINUE, GUIChat::OpenSettings()};
    return {CmdResultCode::INVALID_INPUT, "Unknown settings page " + page};
}

} // namespace GUISettings
