#include "ContractPassUtility.hpp"
#include "ContractTree.hpp"
#include "ErrorMessage.h"
#include <climits>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <optional>
#include <sys/types.h>
#include <vector>

#include "dsa/DSSupport.h"
#include "dsa/Steensgaard.hh"
#include "dsa/DSGraph.h"

using namespace llvm;

// To only warn once if a CB is calling an unknown function
static std::set<const CallBase*> UnknownCalledParam;

std::map<const Value*,int> getFunctionParentInstrCandidates(const Value* Ip) {
    if (!isa<Instruction>(Ip)) return {};
    std::set<std::pair<const Instruction*,int>> candidates = {{dyn_cast<Instruction>(Ip), 0}};
    std::map<const Value*,int> candidatesConsidered;
    while (!candidates.empty()) {
        const Instruction* I = candidates.begin()->first;
        int curSteps = candidates.begin()->second;
        candidatesConsidered.insert({candidates.begin()->first, candidates.begin()->second});
        candidates.erase(candidates.begin());
        while (getPointerOperand(I) && (isa<GlobalValue>(getPointerOperand(I)) || isa<Instruction>(getPointerOperand(I)))) {
            if (isa<GlobalValue>(getPointerOperand(I))) {
                candidatesConsidered.insert({getPointerOperand(I), curSteps});
                return candidatesConsidered;
            } else {
                if (!isa<GetElementPtrInst>(getPointerOperand(I))) curSteps++;
                I = dyn_cast<Instruction>(getPointerOperand(I));
            }
        }
        if (const AllocaInst* AI = dyn_cast<AllocaInst>(I)) {
            // Possibly a function parameter, check args against storeinst users
            const Function* tmp = AI->getParent()->getParent();
            for (int i = 0; i < tmp->arg_size(); i++) {
                for (const User* UA : AI->users() ) {
                    if (!isa<StoreInst>(UA)) continue;
                    if (tmp->getArg(i) != dyn_cast<StoreInst>(UA)->getValueOperand()) continue;
                    // Yup, is an argument
                    for (const User* U : tmp->users()) {
                        if (const CallBase* CB = dyn_cast<CallBase>(U)) {
                            // Callsite with correct argument
                            int offset = 0;
                            if (CB->getCalledFunction()->getName() == "__kmpc_fork_call")
                                offset = 1;
                            if (const Instruction* cI = dyn_cast<Instruction>(CB->getArgOperand(i + offset))) {
                                if (!candidatesConsidered.contains(cI)) {
                                    candidates.insert({cI, --curSteps});   
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return candidatesConsidered;
}

int resolveFunctionDifference(const Value** A, const Value** B) {
    std::map<const Value*,int> CandidatesA = getFunctionParentInstrCandidates(*A);
    std::map<const Value*,int> CandidatesB = getFunctionParentInstrCandidates(*B);
    for (std::pair<const Value*, int> Av : CandidatesA) {
        for (std::pair<const Value*, int> Bv : CandidatesB) {
            if (Av.first == Bv.first) {
                *A = Av.first;
                *B = Bv.first;
                return Av.second - Bv.second;
            }
            if (isa<GlobalValue>(Av.first)) {
                *A = Av.first;
                return Av.second - Bv.second;
            }
            if (isa<GlobalValue>(Bv.first)) {
                *B = Bv.first;
                return Av.second - Bv.second;
            }
            if (isa<Instruction>(Av.first) && isa<Instruction>(Bv.first)) {
                const Instruction* IAv = dyn_cast<Instruction>(Av.first);
                const Instruction* IBv = dyn_cast<Instruction>(Bv.first);
                if (IAv->getParent()->getParent() == IBv->getParent()->getParent()) {
                    *A = IAv;
                    *B = IBv;
                    return Av.second - Bv.second;
                }
            }
        }
    }
    return INT_MAX;
}

namespace ContractPassUtility {

std::optional<uint> getLineNumber(const Instruction* I) {
    if (const DebugLoc& N = I->getDebugLoc()) {
        return N.getLine();
    }
    return std::nullopt;
}
std::string getFile(const Instruction* I) {
    if (I->getDebugLoc())
        return (I->getDebugLoc()->getDirectory() != "" ? I->getDebugLoc()->getDirectory() + "/" : "").str() + I->getDebugLoc()->getFilename().str();
    return "UNKNOWN";
}


std::string getInstrLocStr(const Instruction* I) {
    if (const DebugLoc &debugLoc = I->getDebugLoc())
        return getFile(I) + ":" + std::to_string(debugLoc.getLine()) + ":" + std::to_string(debugLoc->getColumn());
    return "UNKNOWN";
}

FileReference getFileReference(const Instruction* I) {
    if (!I->getDebugLoc())
        errs() << "Warning: Attempting to get file reference of instruction without debug information (most likely caused by outdated LLVM version)!\nThis may have adverse effects on report quality!\n";
    return {
        .file = getFile(I),
        .line = I->getDebugLoc() ? I->getDebugLoc()->getLine() : 0,
        .column = I->getDebugLoc() ? I->getDebugLoc()->getColumn() : 0
    };
}

bool checkCalledApplies(const CallBase* CB, const std::string Target, bool isTag, std::map<const Function*, std::vector<ContractTree::TagUnit>> Tags) {
    if (!isTag) {
        if (!CB->getCalledFunction()) {
            if (!UnknownCalledParam.contains(CB)) {
                errs() << "Could not get name for function at " << getInstrLocStr(CB) << "!\nAnalysis performance is impaired!\n";
                UnknownCalledParam.insert(CB);
            }
            return false;
        }
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

bool checkParamMatch(const Value* contrP, const Value* callP, ContractTree::ParamAccess acc, ModuleAnalysisManager* MAM) {
    const Value* source = contrP;
    const Value* target = callP;
    Module* M = dyn_cast<Instruction>((Value*)contrP)->getModule();
    std::shared_ptr<DSGraph> steens = MAM->getResult<SteensgaardDataStructures>(*M);
    //steens->writeGraphToFile(errs(), "graph");
    int diff = 0;

    switch (acc) {
        case ContractTree::ParamAccess::NORMAL:
            if (diff != 0) return false; // Interproc with load inside
            break;
        case ContractTree::ParamAccess::DEREF:
            // Contr has a pointer, call has value.
            // If interproc, diff should be -1 if already resolved
            if (diff == 0)
                target = getLoadStorePointerOperand(target);
            else if (diff != -1) return false;
            break;
        case ContractTree::ParamAccess::ADDROF:
            // Contr has value, call has pointer. Go down from target param
            // If interproc, diff should be 1 if already resolved
            if (diff == 0)
                source = getLoadStorePointerOperand(source);
            else if (diff != 1) return false;
            break;
    }

    if (source == target) return true;

    if (steens->hasNodeForValue(source) && steens->hasNodeForValue(target)) {
        DSNodeHandle sourceNode = steens->getNodeForValue(source);
        DSNodeHandle targetNode = steens->getNodeForValue(target);
        return sourceNode == targetNode;
    }
    return false;
}

bool checkCallParamApplies(const CallBase* Source, const CallBase* Target, const std::string TargetStr, ContractTree::CallParam const& P, std::map<const Function*, std::vector<ContractTree::TagUnit>> Tags, ModuleAnalysisManager* MAM) {
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
        return checkParamMatch(sourceParam, candidateParam, P.contrParamAccess, MAM);
    }
    return false;
}

}
