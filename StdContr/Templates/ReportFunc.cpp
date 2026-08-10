#include <regex>
#ifdef __clang__
#define __cpp_lib_contracts
#endif
#include <contracts>
#include <iostream>
#include <stacktrace>

void handle_contract_violation(std::contracts::contract_violation v) {
    std::cout << "## Contract Violation detected! ##\n";
    std::string func = v.location().function_name();
    std::cout << "At function call to " << (func.empty() ? "unknown function" : std::regex_replace(func, std::regex(R"(\bCoVer_Wrapper_)"), ""))  << "\n";
    std::cout << "Call Location: " << std::stacktrace::current().at(3).source_file() << ":" << std::stacktrace::current().at(3).source_line() << "\n";
    std::cout << "Contract String: " << v.comment() << "\n";
    std::cout << "Full Stacktrace:\n" << std::stacktrace::current(3) << "\n"; 
    if (v.is_terminating()) {
        std::cout << "Execution will now abort!\n";
    }
}
