#include <ostream>
#ifdef __clang__
#define __cpp_lib_contracts
#endif
#include <contracts>
#include <cstdlib>
#include <cxxabi.h>
#include <format>
#include <iostream>
#include <ranges>
#include <stacktrace>
#include <string>
#include <string_view>

#include "ContractReport.hpp"

extern int8_t COVER_EXIT_SENTINEL;

namespace {

std::string wrapped_function_name(std::string_view signature) {
    constexpr std::string_view wrapper_prefix = "CoVer_Wrapper_";
    std::size_t const start = signature.find(wrapper_prefix);
    if (start == std::string_view::npos) return std::string(signature);
    std::string_view name = signature.substr(start + wrapper_prefix.size());
    name = name.substr(0, name.find('('));

    // Demangle C++ names only
    if (!name.starts_with("_Z")) return std::string(name);
    std::string result(name);
    int status = 0;
    char* demangled = abi::__cxa_demangle(result.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled) result = demangled;
    std::free(demangled);
    return result;
}

std::ostream& out() {
    return std::cerr << "CoVer-StdCXX: ";
}

}

void handle_contract_violation(std::contracts::contract_violation const& v) noexcept {
    if (!cr::has_report()) return; // Contract violation did not come from CoVer

    std::flush(std::cout);
    out() << "## Contract violation detected! ##\n";
    if (!COVER_EXIT_SENTINEL) {
        std::string const func = wrapped_function_name(v.location().function_name());
        out() << "At function call to " << (func.empty() ? "unknown function" : func) << "\n";
        out() << "Call Location: " << std::stacktrace::current()[0].source_file() << ":" << std::stacktrace::current()[0].source_line() << "\n";
    } else {
        out() << "Detected at program exit\n";
    }
    out() << "Full Stacktrace:\n";
    std::stacktrace const trace = std::stacktrace::current(3);
    for (auto const& [i, entry] : std::views::enumerate(trace)) {
        out() << std::format("{:>4}# {}\n", i, entry);
    }

    out() << "Detected " << cr::core_total() << " failing subformula(s):\n";
    for (std::size_t i = 0; i < cr::core_size(); ++i) {
        out() << "--> Subformula " << i << ":\n";
        const cr::leaf failed = cr::core(i);
        out() << "    " << failed.formula.data << "\n";
        if (failed.message.size != 0) out() << "    Message: " << failed.message.data << "\n";
    }
    if (!cr::core_complete()) {
        out() << "--> Only the first " << cr::core_size() << " are shown, define CR_MAX_LEAVES to a larger value for more\n";
    }
    cr::clear_report();
    std::flush(std::cerr);
}
