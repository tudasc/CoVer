#include "TUITrace.hpp"
#include <string>
#include <vector>

namespace TUITrace {

bool verifyInputArgs(std::string_view const& usage, std::string_view const& input, std::vector<std::string>& args, int const& num_inputs) {
    std::istringstream iss(std::string(input), std::ios_base::in);
    // Throw away first input, which is the command itself
    iss.ignore(std::numeric_limits<std::streamsize>::max(), ' ');
    while (iss && args.size() <= num_inputs) {
        std::string x;
        iss >> x;
        if (!iss) break;
        args.push_back(x);
    }
    if (args.size() != num_inputs)  return false;
    return true;
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
