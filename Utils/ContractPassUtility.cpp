#include "ContractPassUtility.hpp"
#include "ContractTree.hpp"
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <map>
#include <string>
#include <optional>
#include <sys/types.h>
#include <vector>

using namespace llvm;

namespace ContractPassUtility {

std::optional<uint> getLineNumber(const Instruction* I) {
    if (const DebugLoc& N = I->getDebugLoc()) {
        return N.getLine();
    }
    return std::nullopt;
}
std::string getInstrLocStr(const Instruction* I) {
    return demangle(I->getParent()->getParent()->getName()) + ":" + (getLineNumber(I).has_value() ? std::to_string(getLineNumber(I).value()) : "UNKNOWN");
}

bool checkCalledApplies(const CallBase* CB, const std::string Target, bool isTag, std::map<const Function*, std::vector<ContractTree::TagUnit>> Tags) {
    if (!isTag) {
        return CB->getCalledFunction()->getName() == Target;
    } else {
        if (!Tags.contains(CB->getCalledFunction())) return false;
        for (const ContractTree::TagUnit tag : Tags[CB->getCalledFunction()]) {
            if (tag.tag == Target) {
                return true;
            }
        }
        return false;
    }
}

bool checkParamMatch(const Value* contrP, const Value* callP, ContractTree::ParamAccess acc) {
    const Value* source;
    const Value* target;
    switch (acc) {
        case ContractTree::ParamAccess::NORMAL:
            source = contrP;
            target = callP;
            break;
        case ContractTree::ParamAccess::DEREF:
            // Contr has a pointer, call has value.
            source = contrP;
            target = getLoadStorePointerOperand(callP);
            break;
        case ContractTree::ParamAccess::ADDROF:
            // Contr has value, call has pointer. Go down from target param
            source = getLoadStorePointerOperand(contrP);
            target = callP;
            break;
        }
    // Now, need to check if they are equal / aliases
    while (true) {
        // If equal, success
        if (source == target) return true;
        // If one is a GEP, resolve "for free"
        if (isa<GetElementPtrInst>(source))
            source = getPointerOperand(source);
        if (isa<GetElementPtrInst>(target))
            target = getPointerOperand(target);
        // Check again, may be equal if synchronized already (i.e. stack array)
        if (source == target) return true;
        // Get their ptr operands if they exist and check again
        source = getPointerOperand(source);
        target = getPointerOperand(target);
        // If one does not have a pointer operand, then they can not match
        if (!source || !target) return false;
    }
}

bool checkCallParamApplies(const CallBase* Source, const CallBase* Target, const std::string TargetStr, ContractTree::CallParam const& P, std::map<const Function*, std::vector<ContractTree::TagUnit>> Tags) {
    std::vector<const Value*> candidateParams;
    const Value* sourceParam = Source->getArgOperand(P.contrP);

    if (!P.callPisTagVar) {
        candidateParams.push_back(Target->getArgOperand(P.callP));
    } else {
        for (ContractTree::TagUnit TagU : Tags[Target->getCalledFunction()]) {
            if (TagU.tag != TargetStr) continue;
            if (!TagU.param.has_value()) continue;
            candidateParams.push_back(Target->getArgOperand(*TagU.param));
        }
        if (candidateParams.empty())
            throw "Could not find candidate parameter with matching tag! Invalid contract definition";
    }

    for (const Value* candidateParam : candidateParams) {
        return checkParamMatch(sourceParam, candidateParam, P.contrParamAccess);
    }
    return false;
}

}