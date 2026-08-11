#include <ostream>
#include <regex>
#ifdef __clang__
#define __cpp_lib_contracts
#endif
#include <contracts>
#include <iostream>
#include <stacktrace>

#include "ContractReport.hpp"

extern int8_t COVER_EXIT_SENTINEL;

void handle_contract_violation(std::contracts::contract_violation const& v) noexcept {
    if (!cr::has_report()) return; // Contract violation did not come from CoVer
    std::flush(std::cout);
    std::flush(std::cerr);
    std::cerr << "## Contract Violation detected! ##\n";
    if (!COVER_EXIT_SENTINEL) {
        std::string func = v.location().function_name();
        std::cerr << "At function call to " << (func.empty() ? "unknown function" : std::regex_replace(func, std::regex(R"(CoVer_Wrapper_)"), ""))  << "\n";
        std::cerr << "Call Location: " << std::stacktrace::current().at(3).source_file() << ":" << std::stacktrace::current().at(3).source_line() << "\n";
    } else {
        std::cerr << "Detected at program exit\n";
    }
    std::string predicate = cr::clean_predicate(v.comment());
    std::cerr << "Transformed contract String: " << predicate << "\n";
    std::cerr << "Full Stacktrace:\n" << std::stacktrace::current(3) << "\n";

    std::cerr << "Detected " << cr::core_size() << " failing subformula(s):\n";
    for (std::size_t i = 0; i < cr::core_size(); ++i) {
        std::cerr << "--> Subformula " << i << ":\n";
        const cr::text message = cr::core(i);
        std::cerr << "    " << message.data << "\n";
    }
    if (!cr::core_complete()) {
        #warning TODO
        std::cerr << "TODO FIXME\n";
    }
    cr::clear_report();

    if (v.is_terminating()) {
        std::cerr << "Execution will now abort!\n";
    }
    std::flush(std::cerr);
}
