#include "TUITrace.hpp"

namespace TUITrace {

std::string verifyInputArgs(std::string const& usage, std::string const& input, std::vector<int>& args, int const& num_inputs) {
    std::istringstream iss(input);
    // Throw away first input, which is the command itself
    iss.ignore(std::numeric_limits<std::streamsize>::max(), ' ');
    while (iss && args.size() <= num_inputs) {
        int x;
        iss >> x;
        if (!iss) break;
        args.push_back(x);
    }
    if (args.size() != num_inputs)  return "Invalid syntax for command. " + usage;
    return "";
}

std::string traceKindToStr(ContractPassUtility::TraceKind kind) {
    switch (kind) {
        case ContractPassUtility::TraceKind::LINEAR: return "LINEAR";
        case ContractPassUtility::TraceKind::BRANCH: return "BRANCH";
        case ContractPassUtility::TraceKind::FUNCENTRY: return "FUNCENTRY";
        case ContractPassUtility::TraceKind::FUNCEXIT: return "FUNCEXIT";
    }
}

}
