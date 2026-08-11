#include <ostream>
#include <regex>
#ifdef __clang__
#define __cpp_lib_contracts
#endif
#include <contracts>
#include <iostream>
#include <stacktrace>

extern int8_t COVER_EXIT_SENTINEL;

void handle_contract_violation(std::contracts::contract_violation v) {
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
    std::cerr << "Contract String: " << v.comment() << "\n";
    std::cerr << "Full Stacktrace:\n" << std::stacktrace::current(3) << "\n"; 
    if (v.is_terminating()) {
        std::cerr << "Execution will now abort!\n";
    }
    std::flush(std::cerr);
}
