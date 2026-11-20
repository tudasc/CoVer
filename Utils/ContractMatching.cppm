module;

#include "ContractTree.hpp"
#include <climits>
#include <cstdlib>

#include "dsa/DSNode.h"
#include "dsa/DSSupport.h"
#include "dsa/Steensgaard.hh"
#include "dsa/DSGraph.h"

// Whether calls and their parameters match those named by a contract, e.g. whether a later call releases
// the buffer passed to the contract's call. Considers the alias and function pointer annotations
export module ContractMatching;
import std;
import LLVMModule;
import ContractPassUtility;
import DeterminizeHelper;

using namespace llvm;

namespace ContractMatching {

// To only warn once if a CB is calling an unknown function
static std::set<const CallBase*> UnknownCalledParam;

// The value both are derived from by following GEPs and the pointer operands of loads, null if there is none
Value const* sameOrigin(Value const* source, Value const* target) {
    while (true) {
        // If either is null, paramerror or if one does not have a pointer operand, then they can not match
        if (!source || !target) return nullptr;
        // If equal, success
        if (source == target) return source;
        // If one is a GEP, resolve "for free"
        if (isa<GEPOperator>(source))
            source = ContractPassUtility::betterGetPointerOperand(source);
        if (isa<GEPOperator>(target))
            target = ContractPassUtility::betterGetPointerOperand(target);
        // Check again, may be equal if synchronized already (i.e. stack array)
        if (source == target) return source;
        // Get their ptr operands if they exist and check again
        source = ContractPassUtility::betterGetPointerOperand(source);
        target = ContractPassUtility::betterGetPointerOperand(target);
    }
}

// Members of the alias group that V refers to: The annotated value itself or its pointer operand.
// Otherwise, equivalent values: For source level annotations, the annotation and each use of a variable
// load it separately
std::set<Value const*> getMatchingAliasMembers(ContractPassUtility::AliasGroup const& AG, Value const* V) {
    std::set<Value const*> res;
    for (Value const* M : AG.members) {
        if (M == V || M == ContractPassUtility::betterGetPointerOperand(V)) res.insert(M);
    }
    if (!res.empty()) return res;
    for (Value const* M : AG.members) {
        if (ContractPassUtility::equivalentValues(M, V)) res.insert(M);
    }
    return res;
}

// Ordered deterministically: The candidate search stops at the first match
using CandidateMap = std::map<const Value*, int, DeterminizeHelper::Less>;
using CandidateSet = std::set<std::pair<const Value*, int>, DeterminizeHelper::Less>;

CandidateSet resolveArgForFuncDiff(Value const* I, int curSteps, CandidateMap const& candidatesConsidered) {
    // First step: Collect the arguments represented by the value
    std::set<Argument const*> args;
    if (const Argument* Arg = dyn_cast<Argument>(I)) {
        args.insert(Arg);
    }
    CandidateSet candidates;
    if (const AllocaInst* AI = dyn_cast<AllocaInst>(I)) {
        // Pre-sroa: Possibly a function parameter, check args against storeinst users
        const Function* tmp = AI->getFunction();
        std::vector<const StoreInst*> stores;
        for (const User* UA : AI->users() ) {
            if (const StoreInst* SI = dyn_cast<StoreInst>(UA); SI && SI->getPointerOperand() == AI) stores.push_back(SI);
        }
        for (const StoreInst* SI : stores) {
            Value const* potentialArg = SI->getValueOperand();
            if (Argument const* Arg = dyn_cast<Argument>(potentialArg)) {
                args.insert(Arg);
                curSteps--;
                break;
            }
        }
        // Local variable holding a single value (e.g. ptr = malloc(...)): Continue with the stored value
        if (args.empty() && stores.size() == 1) {
            if (const Instruction* stored = dyn_cast<Instruction>(stores[0]->getValueOperand()); stored && !candidatesConsidered.contains(stored)) {
                candidates.insert({stored, curSteps - 1});
            }
        }
    }

    // Second step: extract callsites and add to candidate list
    for (Argument const* Arg : args) {
        Function const* F = Arg->getParent();
        // Resolve func aliases
        std::vector<const User*> callers(F->user_begin(), F->user_end());
        for (const User* U : F->users()) {
            if (const GlobalAlias* GA = dyn_cast<GlobalAlias>(U)) callers.insert(callers.end(), GA->user_begin(), GA->user_end());
        }
        for (const User* U : callers) {
            if (const CallBase* CB = dyn_cast<CallBase>(U)) {
                if (ContractPassUtility::getCalleeThroughAlias(CB) != F) continue;
                // Callsite with correct argument
                int offset = 0;
                if (CB->getCalledFunction() && CB->getCalledFunction()->getName() == "__kmpc_fork_call")
                    offset = 1;
                if (const Instruction* cI = dyn_cast<Instruction>(CB->getArgOperand(Arg->getArgNo() + offset))) {
                    if (!candidatesConsidered.contains(cI)) {
                        candidates.insert({cI, curSteps});   
                    }
                }
            }
        }
        for (CallBase* indirectCall : ContractPassUtility::getAnnotatedIndirectCalls(F)) {
            if (const Instruction* cI = dyn_cast<Instruction>(indirectCall->getArgOperand(Arg->getArgNo()))) {
                if (!candidatesConsidered.contains(cI)) {
                    candidates.insert({cI, curSteps});   
                }
            }
        }
    }
    return candidates;
}

CandidateMap getFunctionParentInstrCandidatesUncached(const Value* Ip) {
    if (!isa<Instruction>(Ip) && !isa<Argument>(Ip)) return {};
    CandidateSet candidates = {{Ip, 0}};
    CandidateMap candidatesConsidered;
    while (!candidates.empty()) {
        const Value* I = candidates.begin()->first;
        int curSteps = candidates.begin()->second;
        candidatesConsidered.insert({candidates.begin()->first, candidates.begin()->second});
        candidates.erase(candidates.begin());
        while (ContractPassUtility::betterGetPointerOperand(I) && (isa<GlobalValue>(ContractPassUtility::betterGetPointerOperand(I)) || isa<Instruction>(ContractPassUtility::betterGetPointerOperand(I)))) {
            if (isa<GlobalValue>(ContractPassUtility::betterGetPointerOperand(I))) {
                candidatesConsidered.insert({ContractPassUtility::betterGetPointerOperand(I), curSteps});
                return candidatesConsidered;
            } else {
                if (!isa<GetElementPtrInst>(ContractPassUtility::betterGetPointerOperand(I))) curSteps++;
                I = dyn_cast<Instruction>(ContractPassUtility::betterGetPointerOperand(I));
            }
        }
        CandidateSet parentFuncCandidates = resolveArgForFuncDiff(I, curSteps, candidatesConsidered);
        candidates.insert(parentFuncCandidates.begin(), parentFuncCandidates.end());
    }
    return candidatesConsidered;
}

static DenseMap<const Value*, std::shared_ptr<const CandidateMap>> parentInstrCandidatesCache;

// Drop the cached call chains. To be called after ContractPassUtility::Initialize and after ContractPassUtility::setFPTarget
export void Initialize() {
    parentInstrCandidatesCache.clear();
}

std::shared_ptr<const CandidateMap> getFunctionParentInstrCandidates(const Value* Ip) {
    std::shared_ptr<const CandidateMap>& entry = parentInstrCandidatesCache[Ip];
    if (!entry) entry = std::make_shared<const CandidateMap>(getFunctionParentInstrCandidatesUncached(Ip));
    return entry;
}

int resolveFunctionDifference(const Value** A, const Value** B) {
    std::shared_ptr<const CandidateMap> CandidatesA = getFunctionParentInstrCandidates(*A);
    std::shared_ptr<const CandidateMap> CandidatesB = getFunctionParentInstrCandidates(*B);
    for (std::pair<const Value* const, int> const& Av : *CandidatesA) {
        for (std::pair<const Value* const, int> const& Bv : *CandidatesB) {
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
                if (IAv->getFunction() == IBv->getFunction()) {
                    *A = IAv;
                    *B = IBv;
                    return Av.second - Bv.second;
                }
            }
        }
    }
    return INT_MAX;
}

export bool checkCalledApplies(const CallBase* CB, const StringRef Target, bool isTag, std::map<Function*, std::vector<ContractTree::TagUnit>> const& Tags) {
    std::set<Function*> fns;
    if (CB->isIndirectCall()) fns = ContractPassUtility::getFPAnnots(CB);
    else fns = {ContractPassUtility::getCalleeThroughAlias(CB)};
    if (fns.empty()) {
        if (!UnknownCalledParam.contains(CB)) {
            errs() << "Could not identify function at " << ContractPassUtility::getInstrLocStr(CB) << "!\nAnalysis performance is impaired!\n";
            UnknownCalledParam.insert(CB);
        }
        return false;
    }
    if (!isTag) {
        for (Function* F : fns) {
            if (F->getName() == Target ||  // C-style match
                F->getName() == Target.lower() + "_") { // Fortran-style match
                return true;
            }
        }
        return false;
    } else {
        for (Function* F : fns) {
            if (!Tags.contains(F)) continue;
            for (const ContractTree::TagUnit tag : Tags.at(F)) {
                if (tag.tag == Target) {
                    return true;
                }
            }
        }
        return false;
    }
}

Module* getModule(Value const* V) {
    if (Instruction const* I = dyn_cast<Instruction>(V)) return (Module*)I->getModule();
    if (GlobalValue const* GV = dyn_cast<GlobalValue>(V)) return (Module*)GV->getParent();
    return nullptr;
}

// A value with its function, e.g. "%5 in main"
std::string describeValue(Value const* V) {
    Function const* F = ContractPassUtility::getParentFunction(V);
    return F ? std::format("{} in {}", ContractPassUtility::getValueName(V), F->getName().str()) : ContractPassUtility::getValueName(V);
}

// A member of an alias group as listed by alias-get, e.g. "3.012 (%5 in main)"
std::string describeMember(int group, ContractPassUtility::AliasGroup const& AG, Value const* M) {
    std::set<Value*, std::less<>>::const_iterator it = AG.members.find(M);
    return std::format("{}.{:0>3} ({})", group, std::distance(AG.members.begin(), it), describeValue(M));
}

// The decision of checkParamMatch with what it is based on. The text is only built by Explain, so that the analysis does not pay for it
export struct ParamMatch {
    enum struct Reason {
        SROA_STORE, // Stores created by SROA are never matched
        UNCONNECTED, // In different functions, not passed from one to the other through call arguments
        INDIRECTION, // A different number of loads on the way through the calls
        SAME_VALUE,
        ALIAS_GROUP, // group, source_member, target_member
        NO_ALIAS_GROUP, // Separated by group, source_member, target_member
        DSA, // Decided by the DSA alias analysis (Fortran)
        SAME_ORIGIN, // origin
        DIFFERENT_ORIGINS,
    };

    bool match = false;
    Reason reason = Reason::DIFFERENT_ORIGINS;
    Value const* source = nullptr; // The compared values, followed through the call arguments if through_calls
    Value const* target = nullptr;
    bool through_calls = false;
    int group = -1;
    Value const* source_member = nullptr;
    Value const* target_member = nullptr;
    Value const* origin = nullptr;
    std::vector<std::pair<int, Value const*>> unseparated; // No-alias groups checked before, in which both match the same member

    explicit operator bool() const { return match; }

    std::string Explain() const {
        std::string out;
        if (through_calls) out += std::format("Compared through the call arguments as {} and {}. ", describeValue(source), describeValue(target));
        for (auto const& [g, member] : unseparated)
            out += std::format("No-alias group {} does not separate them, both match its member {}. ", g, describeMember(g, ContractPassUtility::getAliasAnnots().at(g), member));
        switch (reason) {
            case Reason::SROA_STORE: return out + "Stores created by SROA are never matched";
            case Reason::UNCONNECTED: return out + "The values are in different functions, and are not passed from one to the other through call arguments";
            case Reason::INDIRECTION: return out + "They differ in the number of loads on the way through the calls";
            case Reason::SAME_VALUE: return out + "Same value";
            case Reason::ALIAS_GROUP:
                return out + std::format("Alias group {} asserts that they alias, they match its members {} and {}", group,
                                         describeMember(group, ContractPassUtility::getAliasAnnots().at(group), source_member), describeMember(group, ContractPassUtility::getAliasAnnots().at(group), target_member));
            case Reason::NO_ALIAS_GROUP:
                return out + std::format("No-alias group {} separates them, they match its members {} and {}", group,
                                         describeMember(group, ContractPassUtility::getAliasAnnots().at(group), source_member), describeMember(group, ContractPassUtility::getAliasAnnots().at(group), target_member));
            case Reason::DSA: return out + "Decided by the DSA alias analysis used for Fortran";
            case Reason::SAME_ORIGIN: return out + std::format("Same origin: both are derived from {} by GEPs and loads", describeValue(origin));
            case Reason::DIFFERENT_ORIGINS: return out + "Different origins: They derive from no common value";
        }
        return out;
    }
};

// Whether the analysis takes the contract parameter contrP and callP, accessed as acc says, as the same value, considering the current annotations
export ParamMatch checkParamMatch(const Value* contrP, const Value* callP, ContractTree::ParamAccess acc = ContractTree::ParamAccess::NORMAL) {
    using Reason = ParamMatch::Reason;
    ParamMatch res;
    const Value* source = contrP;
    const Value* target = callP;
    int diff = 0;

    // Filter out new instr from sroa
    if (isa<StoreInst>(callP) && dyn_cast<StoreInst>(callP)->getPointerOperand()->getName().starts_with(".fca.") ||
        isa<StoreInst>(contrP) && dyn_cast<StoreInst>(contrP)->getPointerOperand()->getName().starts_with(".fca.")) {
        res.reason = Reason::SROA_STORE;
        return res;
    }

    // Only use DSA for Fortran
    if (!ContractPassUtility::isFortran()) {
        // Resolve function differences.
        // If one is a global, this does not matter, so check if they have a parent function first
        const Function* Fs = ContractPassUtility::getParentFunction(source);
        const Function* Ft = ContractPassUtility::getParentFunction(target);
        if (Fs && Ft && Fs != Ft) {
            // Definitely different functions
            diff = resolveFunctionDifference(&source, &target);
            if (diff == INT_MAX) {
                res.reason = Reason::UNCONNECTED;
                return res;
            }
            res.through_calls = true;
            res.source = source;
            res.target = target;
        }
    }

    switch (acc) {
        case ContractTree::ParamAccess::NORMAL:
            if (diff != 0) { // Interproc with load inside
                res.reason = Reason::INDIRECTION;
                return res;
            }
            break;
        case ContractTree::ParamAccess::DEREF:
            // Contr has a pointer, call has value.
            // If interproc, diff should be -1 if already resolved
            if (diff == 0) {
                Value const* V = getLoadStorePointerOperand(target);
                if (!V && ContractPassUtility::isDebug()) WithColor(errs(), HighlightColor::String) << "Note: Static deref failed, falling back to orig.\n";
                target = V ? V : target;
            }
            else if (diff != -1) {
                res.reason = Reason::INDIRECTION;
                return res;
            }
            break;
        case ContractTree::ParamAccess::ADDROF:
            // Contr has value, call has pointer. Go down from target param
            // If interproc, diff should be 1 if already resolved
            if (diff == 0)
                source = getLoadStorePointerOperand(source);
            else if (diff != 1) {
                res.reason = Reason::INDIRECTION;
                return res;
            }
            break;
    }

    if (source == target) {
        res.match = true;
        res.reason = Reason::SAME_VALUE;
        return res;
    }

    // Check for alias annotations: Both need to refer to members of the same alias group
    if (source && target) {
        for (std::pair<const int, ContractPassUtility::AliasGroup> const& AG : ContractPassUtility::getAliasAnnots()) {
            std::set<Value const*> srcMembers = getMatchingAliasMembers(AG.second, source);
            if (srcMembers.empty()) continue;
            std::set<Value const*> tgtMembers = getMatchingAliasMembers(AG.second, target);
            if (tgtMembers.empty()) continue;
            res.group = AG.first;
            res.source_member = *srcMembers.begin();
            res.target_member = *tgtMembers.begin();
            if (AG.second.areAliasing) {
                res.match = true;
                res.reason = Reason::ALIAS_GROUP;
                return res;
            }
            // No-alias groups only separate different members, not two uses of the same annotated value
            std::set<Value const*>::const_iterator shared = std::ranges::find_if(srcMembers, [&tgtMembers](Value const* M) { return tgtMembers.contains(M); });
            if (shared == srcMembers.end()) {
                res.reason = Reason::NO_ALIAS_GROUP;
                return res;
            }
            res.unseparated.push_back({AG.first, *shared});
        }
    }

    if (ContractPassUtility::isFortran()) {
        res.reason = Reason::DSA;
        std::shared_ptr<DSGraph> steens = ContractPassUtility::getModuleAnalyses().getResult<SteensgaardDataStructures>(*getModule(contrP));
        if (steens->hasNodeForValue(source) && steens->hasNodeForValue(target)) {
            DSNodeHandle sourceNode = steens->getNodeForValue(source);
            DSNodeHandle targetNode = steens->getNodeForValue(target);
            while (sourceNode.getNode()->isCollapsedNode())
                sourceNode = sourceNode.getNode()->edge_begin()->second;
            while (targetNode.getNode()->isCollapsedNode())
                targetNode = targetNode.getNode()->edge_begin()->second;
            res.match = sourceNode == targetNode;
        }
        return res;
    }
    // Now, need to check if they are equal / aliases
    res.origin = sameOrigin(source, target);
    res.match = res.origin != nullptr;
    res.reason = res.match ? Reason::SAME_ORIGIN : Reason::DIFFERENT_ORIGINS;
    return res;
}

export bool checkCallParamApplies(const CallBase* Source, const CallBase* Target, const std::string TargetStr, ContractTree::CallParam const& P, std::map<Function*, std::vector<ContractTree::TagUnit>> const& Tags) {
    std::vector<const Value*> candidateParams;
    const Value* sourceParam = Source->getArgOperand(P.contrP);

    if (!P.callPisTagVar) {
        candidateParams.push_back(Target->getArgOperand(P.callP));
    } else {
        std::set<Function*> fns;
        if (Target->isIndirectCall()) fns = ContractPassUtility::getFPAnnots(Target);
        else fns = {dyn_cast<Function>(Target->getCalledOperand())};
        for (Function* F : fns) {
            auto tagsIt = Tags.find(F);
            if (tagsIt == Tags.end()) continue;
            for (ContractTree::TagUnit TagU : tagsIt->second) {
                if (TagU.tag != TargetStr) continue;
                if (!TagU.param.has_value()) continue;
                candidateParams.push_back(Target->getArgOperand(*TagU.param));
            }
        }
        if (candidateParams.empty()) {
            errs() << "CoVer: Could not find candidate parameter with matching tag " << TargetStr << " for " << ContractPassUtility::getInstrLocStr(Target) << ". Invalid contract definition!\n";
            std::exit(EXIT_FAILURE);
        }
    }

    for (const Value* candidateParam : candidateParams) {
        return checkParamMatch(sourceParam, candidateParam, P.contrParamAccess).match;
    }
    return false;
}
}
