module;

#include "ContractTree.hpp"
#include <cassert>
#include <climits>
#include <cstdlib>

#include "dsa/DSNode.h"
#include "dsa/DSSupport.h"
#include "dsa/Steensgaard.hh"
#include "dsa/DSGraph.h"

export module ContractPassUtility;
import std;
import LLVMModule;
import ErrorMessage;

using namespace llvm;

// The module to analyse
Module* curM;

// 32 bit integer constant, the type of the int parameters of CoVer's annotation functions
ConstantInt* getInt(int x) { return ConstantInt::get(Type::getInt32Ty(curM->getContext()), x); }

// To only warn once if a CB is calling an unknown function
static std::set<const CallBase*> UnknownCalledParam;

// For language-specific stuff
static bool isFort = false;

// Map from function to indirect calls to it, as gotten by annotations
std::map<const Function*, std::set<CallBase*>> AnnotFuncReverse;

// Whether to output IR or human-readable
bool outputIR = false;

namespace ContractPassUtility {

export struct AliasGroup {
    std::set<Value*, std::less<>> members;
    bool areAliasing;
};

// Annotation infos
std::map<int, ContractPassUtility::AliasGroup> aliasInfo;

// Function analyses of the current module
FunctionAnalysisManager* curFAM = nullptr;

#define DEBUG_ENV "COVER_LLVM_DEBUG"
export bool isDebug() {
    static const bool debug = getenv(DEBUG_ENV) != NULL && atoi(getenv(DEBUG_ENV)) == 1;
    return debug;
}

export const Value* betterGetPointerOperand(const Value* V) {
    const Value* b = getPointerOperand(V);
    if (b == nullptr) {
        if (const GEPOperator* GEPOp = dyn_cast<GEPOperator>(V)) b = GEPOp->getPointerOperand();
    }
    return b;
}

bool sameOriginCheck(Value const* source, Value const* target) {
    while (true) {
        // If either is null, paramerror or if one does not have a pointer operand, then they can not match
        if (!source || !target) return false;
        // If equal, success
        if (source == target) return true;
        // If one is a GEP, resolve "for free"
        if (isa<GEPOperator>(source))
            source = betterGetPointerOperand(source);
        if (isa<GEPOperator>(target))
            target = betterGetPointerOperand(target);
        // Check again, may be equal if synchronized already (i.e. stack array)
        if (source == target) return true;
        // Get their ptr operands if they exist and check again
        source = betterGetPointerOperand(source);
        target = betterGetPointerOperand(target);
    }
}

export std::map<int, AliasGroup> const& getAliasAnnots() { return aliasInfo; }
// The memory state a load reads: The last access that may have written the loaded memory
MemoryAccess const* getReachingDef(LoadInst const* LI) {
    if (!curFAM) return nullptr;
    MemorySSA& MSSA = curFAM->getResult<MemorySSAAnalysis>(*const_cast<Function*>(LI->getFunction())).getMSSA();
    return MSSA.getWalker()->getClobberingMemoryAccess(LI);
}

// IR edits invalidate the memory SSA of the edited function
export void invalidateFunctionAnalyses(Function* F) {
    if (curFAM && F) curFAM->invalidate(*F, PreservedAnalyses::none());
}

// Whether both values are computed the same way from the same values, e.g. two loads of a variable
// without a store to it in between, or two GEPs into the same array with equivalent indices
export bool equivalentValues(Value const* A, Value const* B) {
    if (A == B) return true;
    Instruction const* IA = dyn_cast<Instruction>(A);
    Instruction const* IB = dyn_cast<Instruction>(B);
    if (!IA || !IB) return false;
    if (!isa<LoadInst, GetElementPtrInst, CastInst, BinaryOperator>(IA)) return false;
    if (!IA->isSameOperationAs(IB)) return false;
    if (LoadInst const* LA = dyn_cast<LoadInst>(IA)) {
        // Loads only read the same value if they read the same memory state
        if (IA->getFunction() != IB->getFunction()) return false;
        MemoryAccess const* defA = getReachingDef(LA);
        if (!defA || defA != getReachingDef(cast<LoadInst>(IB))) return false;
    }
    for (unsigned i = 0; i < IA->getNumOperands(); i++) {
        if (!equivalentValues(IA->getOperand(i), IB->getOperand(i))) return false;
    }
    return true;
}

// Members of the alias group that V refers to: The annotated value itself or its pointer operand.
// Otherwise, equivalent values: For source level annotations, the annotation and each use of a variable
// load it separately
std::set<Value const*> getMatchingAliasMembers(AliasGroup const& AG, Value const* V) {
    std::set<Value const*> res;
    for (Value const* M : AG.members) {
        if (M == V || M == betterGetPointerOperand(V)) res.insert(M);
    }
    if (!res.empty()) return res;
    for (Value const* M : AG.members) {
        if (equivalentValues(M, V)) res.insert(M);
    }
    return res;
}

// The function pointer annotation of an indirect call: An annotation dominating the call, annotating the same value as the called one.
// If there are multiple, they are ordered by dominance, and the closest one is used
CallBase* getFPAnnotCall(CallBase const* CB) {
    if (!CB->isIndirectCall() || !curFAM) return nullptr;
    Function* AnnotFP = CB->getModule()->getFunction("CoVer_AnnotFP");
    if (!AnnotFP) return nullptr;
    Function* F = const_cast<Function*>(CB->getFunction());
    DominatorTree& DT = curFAM->getResult<DominatorTreeAnalysis>(*F);
    CallBase* res = nullptr;
    for (User* U : AnnotFP->users()) {
        CallBase* annot = dyn_cast<CallBase>(U);
        if (!annot || annot->getCalledOperand() != AnnotFP || annot->getFunction() != F) continue;
        if (!DT.dominates(annot, CB)) continue;
        if (!equivalentValues(annot->getArgOperand(0), CB->getCalledOperand())) continue;
        if (!res || DT.dominates(res, annot)) res = annot;
    }
    return res;
}

export std::set<Function*> getFPAnnots(CallBase const* CB) {
    CallBase const* annot = getFPAnnotCall(CB);
    if (!annot) return {};
    std::set<Function*> fp_targets;
    for (unsigned i = 2; i < annot->arg_size(); i++) {
        fp_targets.insert(CB->getModule()->getFunction(annot->getArgOperand(i)->getName()));
    }
    return fp_targets;
}

// Called function, also if called through an alias (e.g. C++ complete object constructor aliasing the base object one)
export Function* getCalleeThroughAlias(CallBase const* CB) {
    return dyn_cast<Function>(CB->getCalledOperand()->stripPointerCastsAndAliases());
}

Function const* getParentFunction(Value const* V) {
    if (Instruction const* I = dyn_cast<Instruction>(V))
        return I->getFunction();
    if (Argument const* Arg = dyn_cast<Argument>(V))
        return Arg->getParent();
    return nullptr;
}

std::set<std::pair<const Value*, int>> resolveArgForFuncDiff(Value const* I, int curSteps, std::map<const Value*,int> const& candidatesConsidered) {
    // First step: Collect the arguments represented by the value
    std::set<Argument const*> args;
    if (const Argument* Arg = dyn_cast<Argument>(I)) {
        args.insert(Arg);
    }
    std::set<std::pair<const Value*, int>> candidates;
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
                if (getCalleeThroughAlias(CB) != F) continue;
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
        if (AnnotFuncReverse.contains(F)) {
            for (CallBase* indirectCall : AnnotFuncReverse[F]) {
                if (const Instruction* cI = dyn_cast<Instruction>(indirectCall->getArgOperand(Arg->getArgNo()))) {
                    if (!candidatesConsidered.contains(cI)) {
                        candidates.insert({cI, curSteps});   
                    }
                }
            }
        }
    }
    return candidates;
}

std::map<const Value*,int> getFunctionParentInstrCandidatesUncached(const Value* Ip) {
    if (!isa<Instruction>(Ip) && !isa<Argument>(Ip)) return {};
    std::set<std::pair<const Value*,int>> candidates = {{Ip, 0}};
    std::map<const Value*,int> candidatesConsidered;
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
        std::set<std::pair<const Value*,int>> parentFuncCandidates = resolveArgForFuncDiff(I, curSteps, candidatesConsidered);
        candidates.insert(parentFuncCandidates.begin(), parentFuncCandidates.end());
    }
    return candidatesConsidered;
}

static DenseMap<const Value*, std::shared_ptr<const std::map<const Value*,int>>> parentInstrCandidatesCache;

std::shared_ptr<const std::map<const Value*,int>> getFunctionParentInstrCandidates(const Value* Ip) {
    std::shared_ptr<const std::map<const Value*,int>>& entry = parentInstrCandidatesCache[Ip];
    if (!entry) entry = std::make_shared<const std::map<const Value*,int>>(getFunctionParentInstrCandidatesUncached(Ip));
    return entry;
}

int resolveFunctionDifference(const Value** A, const Value** B) {
    std::shared_ptr<const std::map<const Value*,int>> CandidatesA = getFunctionParentInstrCandidates(*A);
    std::shared_ptr<const std::map<const Value*,int>> CandidatesB = getFunctionParentInstrCandidates(*B);
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

void sortBlocks(llvm::Function &F) {
  if (F.isDeclaration() || F.size() <= 1)
    return;

  llvm::ReversePostOrderTraversal<llvm::Function *> RPOT(&F);
  llvm::BasicBlock *Prev = nullptr;
  for (llvm::BasicBlock *BB : RPOT) {
    if (Prev)
      BB->moveAfter(Prev);
    Prev = BB;
  }
}

void determinizeModule() {
    curM->getFunctionList().sort([](const llvm::Function &A, const llvm::Function &B) {
        return A.getName() < B.getName();
    });
    for (Function& F : *curM) {
        sortBlocks(F);
        // Position in the function is index for use list
        DenseMap<BasicBlock const*, int> Order;
        int Idx = 0;
        for (BasicBlock const& BB : F) Order[&BB] = Idx++;

        auto predKey = [&](const Use &U) -> unsigned {
            // A block value's users are terminators; map each back to its parent (the pred).
            if (Instruction* I = dyn_cast<Instruction>(U.getUser())) return Order.lookup(I->getParent());
            return UINT_MAX; // blockaddress / non-terminator users sink to the end
        };

        for (BasicBlock &BB : F) {
            BB.sortUseList([&](const Use &L, const Use &R) {
                return predKey(L) < predKey(R);
            });
        }
    }

    NamedMDNode *CUs = curM->getNamedMetadata("llvm.dbg.cu");
    SmallVector<MDNode *, 8> Nodes(CUs->operands());

    llvm::sort(Nodes, [](MDNode *A, MDNode *B) {
        DICompileUnit* CA = cast<DICompileUnit>(A);
        DICompileUnit* CB = cast<DICompileUnit>(B);
        return CA->getFile()->getFilename() < CB->getFile()->getFilename();
    });

    CUs->clearOperands();
    for (MDNode *N : Nodes)
        CUs->addOperand(N);

    for (DICompileUnit *CU : curM->debug_compile_units()) {
        if (!CU->getGlobalVariables()) continue;
        SmallVector<Metadata *, 16> GVs(CU->getGlobalVariables()->operands());
        llvm::sort(GVs, [](Metadata *A, Metadata *B) {
            auto *GA = cast<DIGlobalVariableExpression>(A)->getVariable();
            auto *GB = cast<DIGlobalVariableExpression>(B)->getVariable();
            return GA->getName() < GB->getName();
        });
        CU->replaceGlobalVariables(MDTuple::get(curM->getContext(), GVs));
    }
}

export StoreInst* getLastStore(CallBase* CB, int idx) {
    Instruction* cur = CB->getPrevNode();
    while (cur) {
        if (isa<CallBase>(cur) && !dyn_cast<CallBase>(cur)->getCalledOperand()->getName().starts_with("PPDCV")) break;
        if (StoreInst* SI = dyn_cast<StoreInst>(cur)) {
            if (SI->getPointerOperand() == CB->getArgOperand(idx)) return SI;
        }
        cur = cur->getPrevNode();
    }
    return nullptr;
}

export void Initialize(Module& M, ModuleAnalysisManager& MAM, bool _outputIR) {

    isFort = M.getFunction("_QQmain");
    curM = &M;

    parentInstrCandidatesCache.clear();
    curFAM = &MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

    outputIR = _outputIR;

    // Function list and basic blocks must be sorted for clean diff on interactive analysis
    determinizeModule();

    Function* AnnotAlias = M.getFunction("CoVer_AnnotAlias");
    for (User* U : AnnotAlias->users()) {
        if (CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledOperand() == AnnotAlias) {
                int groupIdx = dyn_cast<ConstantInt>(CB->getArgOperand(2))->getZExtValue();
                if (!aliasInfo.contains(groupIdx)) aliasInfo[groupIdx];
                aliasInfo[groupIdx].areAliasing = dyn_cast<ConstantInt>(CB->getArgOperand(1))->getZExtValue();
                #warning TODO need to get actual value here maybe
                aliasInfo[groupIdx].members.insert(CB->getArgOperand(0));
            }
        }
    }

    // Collect the annotated targets of all indirect calls, so call chains through them can be resolved independent of analysis order
    AnnotFuncReverse.clear();
    for (Function& F : M) {
        for (Instruction& I : instructions(F)) {
            CallBase* CB = dyn_cast<CallBase>(&I);
            if (!CB || !CB->isIndirectCall()) continue;
            for (Function* Target : getFPAnnots(CB)) {
                if (Target && !Target->isDeclaration()) AnnotFuncReverse[Target].insert(CB);
            }
        }
    }
}

export std::optional<unsigned int> getLineNumber(const Instruction* I) {
    if (const DebugLoc& N = I->getDebugLoc()) {
        return N.getLine();
    }
    return std::nullopt;
}
export std::string getFile(const Instruction* I, bool longform = true) {
    if (I->getDebugLoc())
        return (I->getDebugLoc()->getDirectory() != "" && longform ? I->getDebugLoc()->getDirectory() + "/" : "").str() + I->getDebugLoc()->getFilename().str();
    return "UNKNOWN";
}

// The integer the whole text spells, e.g. a constant contract value, or nothing if it is no (representable) integer
export std::optional<int> parseInt(std::string_view text) {
    int value = 0;
    char const* end = text.data() + text.size();
    std::from_chars_result parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end) return std::nullopt;
    return value;
}

export std::string getInstrLocStr(const Function* I, bool longform = true) {
    if (const DISubprogram* debugLoc = I->getSubprogram()) {
        if (!outputIR) return ((longform ? debugLoc->getDirectory() + "/" : "") + debugLoc->getFilename()).str() + ":" + std::to_string(debugLoc->getLine());
        else return I->getName().str();
    }
    return "UNKNOWN";
}

export std::string getInstrLocStr(const Instruction* I, bool longform = true) {
    if (const DebugLoc &debugLoc = I->getDebugLoc()) {
        if (!outputIR) return getFile(I, longform) + ":" + std::to_string(debugLoc.getLine()) + (longform ? ":" + std::to_string(debugLoc->getColumn()) : "");
        else {
            static llvm::DenseMap<const Instruction*, std::string> cache;
            auto [it, inserted] = cache.try_emplace(I);
            if (inserted) {
                llvm::ModuleSlotTracker MST(I->getModule());
                MST.incorporateFunction(*I->getFunction());
                llvm::raw_string_ostream(it->second) << *I;
            }
            return std::format("IR {} in {}", it->second, I->getFunction()->getName().str());
        }
    }
    return "UNKNOWN";
}

export FileReference getFileReference(const Instruction* I) {
    return {
        .file = getFile(I),
        .line = I->getDebugLoc() ? I->getDebugLoc()->getLine() : 0,
        .column = I->getDebugLoc() ? I->getDebugLoc()->getColumn() : 0
    };
}

export bool isTrivialAlloc(Value const* V) {
    // First possibility: Its a global alloc, trivially fulfilled
    if (GlobalVariable const* GV = dyn_cast<GlobalVariable>(V)) {
        if (isFort) {
            SmallVector<DIGlobalVariableExpression*> dbg_arr;
            GV->getDebugInfo(dbg_arr);
            if (!dbg_arr.empty()) {
                if (DICompositeType* T = dyn_cast<DICompositeType>(dbg_arr[0]->getVariable()->getType())) {
                    if (T->getDataLocationExp()) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    // Second possibility: Its a stack var, trivially fulfilled
    const Value* tmp = V;
    while (isa<GetElementPtrInst>(tmp)) {
        tmp = getPointerOperand(tmp);
    }

    // Stack Variables
    if (AllocaInst const* AI = dyn_cast<AllocaInst>(tmp)) {
        if (isFort) {
            // Fortran will do weird pointer / metadata stuff -> Need to check if stack thingy has external data location
            Instruction const* dbgdeclare = AI;
            while (dbgdeclare->getNextNode() && isa<AllocaInst>(dbgdeclare)) dbgdeclare = dbgdeclare->getNextNode();
            for (DbgRecord const& DR : dbgdeclare->getDbgRecordRange()) {
                if (DbgVariableRecord const* DVR = dyn_cast<DbgVariableRecord>(&DR)) {
                    if (DVR->getAddress() != AI) continue;
                    if (DICompositeType const* T = dyn_cast<DICompositeType>(DVR->getVariable()->getType())) {
                        if (!T->getDataLocationExp()) {
                            return true;
                        }
                    }
                }
            }
        } else {
            // C will just put the stack var in the function like a normal program -> already allocated
            return true;
        }
    }

    // Not trivially allocated
    return false;
}

export ConstantInt* fortCheckAndGetGlbInt(Value* V) {
    if (V->getName().starts_with("_QQ")) {
        if (GlobalVariable const* GV = dyn_cast<GlobalVariable>(V)) {
            if (GV->hasInitializer()) {
                if (StructType const* T = dyn_cast<StructType>(GV->getInitializer()->getType())) {
                    if (T->getNumElements() == 1 && T->getElementType(0)->isIntegerTy()) return dyn_cast<ConstantInt>(GV->getInitializer()->getAggregateElement((unsigned int)0));
                }
            }
        }
    }
    return nullptr;
}

export bool checkCalledApplies(const CallBase* CB, const StringRef Target, bool isTag, std::map<Function*, std::vector<ContractTree::TagUnit>> const& Tags) {
    std::set<Function*> fns;
    if (CB->isIndirectCall()) fns = getFPAnnots(CB);
    else fns = {dyn_cast<Function>(CB->getCalledOperand())};
    if (fns.empty()) {
        if (!UnknownCalledParam.contains(CB)) {
            errs() << "Could not identify function at " << getInstrLocStr(CB) << "!\nAnalysis performance is impaired!\n";
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

export bool checkParamMatch(const Value* contrP, const Value* callP, ContractTree::ParamAccess acc, ModuleAnalysisManager* MAM) {
    const Value* source = contrP;
    const Value* target = callP;
    int diff = 0;

    // Filter out new instr from sroa
    if (isa<StoreInst>(callP) && dyn_cast<StoreInst>(callP)->getPointerOperand()->getName().starts_with(".fca.") ||
        isa<StoreInst>(contrP) && dyn_cast<StoreInst>(contrP)->getPointerOperand()->getName().starts_with(".fca.")) {
        return false;
    }

    // Only use DSA for Fortran
    if (!isFort) {
        // Resolve function differences.
        // If one is a global, this does not matter, so check if they have a parent function first
        const Function* Fs = getParentFunction(source);
        const Function* Ft = getParentFunction(target);
        if (Fs && Ft && Fs != Ft) {
            // Definitely different functions
            diff = resolveFunctionDifference(&source, &target);
            if (diff == INT_MAX) return false;
        }
    }

    switch (acc) {
        case ContractTree::ParamAccess::NORMAL:
            if (diff != 0) return false; // Interproc with load inside
            break;
        case ContractTree::ParamAccess::DEREF:
            // Contr has a pointer, call has value.
            // If interproc, diff should be -1 if already resolved
            if (diff == 0) {
                Value const* V = getLoadStorePointerOperand(target);
                if (!V && isDebug()) WithColor(errs(), HighlightColor::String) << "Note: Static deref failed, falling back to orig.\n";
                target = V ? V : target;
            }
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

    // Check for alias annotations: Both need to refer to members of the same alias group
    if (source && target) {
        for (std::pair<const int, AliasGroup> const& AG : aliasInfo) {
            std::set<Value const*> srcMembers = getMatchingAliasMembers(AG.second, source);
            if (srcMembers.empty()) continue;
            std::set<Value const*> tgtMembers = getMatchingAliasMembers(AG.second, target);
            if (tgtMembers.empty()) continue;
            if (AG.second.areAliasing) return true;
            // No-alias groups only separate different members, not two uses of the same annotated value
            bool sameMember = std::ranges::any_of(srcMembers, [&tgtMembers](Value const* M) { return tgtMembers.contains(M); });
            if (!sameMember) return false;
        }
    }

    if (isFort) {
        std::shared_ptr<DSGraph> steens = MAM->getResult<SteensgaardDataStructures>(*getModule(contrP));
        if (steens->hasNodeForValue(source) && steens->hasNodeForValue(target)) {
            DSNodeHandle sourceNode = steens->getNodeForValue(source);
            DSNodeHandle targetNode = steens->getNodeForValue(target);
            while (sourceNode.getNode()->isCollapsedNode())
                sourceNode = sourceNode.getNode()->edge_begin()->second;
            while (targetNode.getNode()->isCollapsedNode())
                targetNode = targetNode.getNode()->edge_begin()->second;
            return sourceNode == targetNode;
        }
    } else {
        // Now, need to check if they are equal / aliases
        return sameOriginCheck(source, target);
    }
    return false;
}

export bool checkCallParamApplies(const CallBase* Source, const CallBase* Target, const std::string TargetStr, ContractTree::CallParam const& P, std::map<Function*, std::vector<ContractTree::TagUnit>> const& Tags, ModuleAnalysisManager* MAM) {
    std::vector<const Value*> candidateParams;
    const Value* sourceParam = Source->getArgOperand(P.contrP);

    if (!P.callPisTagVar) {
        candidateParams.push_back(Target->getArgOperand(P.callP));
    } else {
        std::set<Function*> fns;
        if (Target->isIndirectCall()) fns = getFPAnnots(Target);
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
            errs() << "CoVer: Could not find candidate parameter with matching tag " << TargetStr << " for " << getInstrLocStr(Target) << ". Invalid contract definition!\n";
            std::exit(EXIT_FAILURE);
        }
    }

    for (const Value* candidateParam : candidateParams) {
        return checkParamMatch(sourceParam, candidateParam, P.contrParamAccess, MAM);
    }
    return false;
}

export Value* getValueByName(std::string name, Function* F) {
    if (name.empty()) return nullptr;
    if (name.starts_with("%")) {
        if (F && !F->isDeclaration()) {
            // Try arguments
            for (Argument& A : F->args()) {
                if (A.getNameOrAsOperand() == name || "%" + A.getNameOrAsOperand() == name) {
                    return &A;
                }
            }
            // Then instructions
            for (BasicBlock& BB : *F) {
                for (Instruction& I : BB) {
                    if (I.getNameOrAsOperand() == name || "%" + I.getNameOrAsOperand() == name) {
                        return &I;
                    }
                }
            }
        }
        return nullptr;
    }
    return F->getParent()->getNamedValue(name.substr(1));
}

export void addToAliasGroup(int idx, Value* V) {
    if (aliasInfo[idx].members.contains(V)) return; // Need to do early return to not double-instrument
    aliasInfo[idx].members.insert(V);
    CallInst* CI = CallInst::Create(curM->getFunction("CoVer_AnnotAlias"), {V, ConstantInt::getBool(curM->getContext(), aliasInfo[idx].areAliasing), getInt(idx)});
    if (Instruction* I = dyn_cast<Instruction>(V)) {
        invalidateFunctionAnalyses(I->getFunction());
        CI->insertAfter(I);
        CI->setDebugLoc(I->getDebugLoc());
    } else if (Argument* A = dyn_cast<Argument>(V)) {
        invalidateFunctionAnalyses(A->getParent());
        CI->insertBefore(A->getParent()->getEntryBlock().getFirstInsertionPt());
    } else {
        invalidateFunctionAnalyses(curM->getFunction("main"));
        CI->insertBefore(curM->getFunction("main")->getEntryBlock().getFirstInsertionPt());
    }
}
export void createAliasGroup(bool shouldAlias, Value* V1, Value* V2) {
    aliasInfo[aliasInfo.empty() ? 0 : aliasInfo.rbegin()->first + 1] = {{}, shouldAlias};
    for (Value* V : {V1, V2}) {
        addToAliasGroup(aliasInfo.rbegin()->first, V);
    }
}
export void setAliasGroupType(int idx, bool isAlias) {
    aliasInfo[idx].areAliasing = isAlias;
    for (User* U : curM->getFunction("CoVer_AnnotAlias")->users()) {
        if (CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledOperand()->getName() != "CoVer_AnnotAlias") continue;
            if (dyn_cast<ConstantInt>(CB->getArgOperand(2))->getZExtValue() != idx) continue;
            CB->setArgOperand(1, ConstantInt::getBool(curM->getContext(), isAlias));
        }
    }
}
// Epoch annotations of an alias group, in module order
export std::vector<CallBase*> getAliasEpochs(int group) {
    std::vector<CallBase*> res;
    for (Function& F : *curM) {
        for (Instruction& I : instructions(F)) {
            if (CallBase* CB = dyn_cast<CallBase>(&I)) {
                if (CB->getCalledOperand()->getName() != "CoVer_AnnotAliasEpoch") continue;
                if (dyn_cast<ConstantInt>(CB->getArgOperand(0))->getSExtValue() == group) res.push_back(CB);
            }
        }
    }
    return res;
}
export void removeAliasEpoch(int group, int idx) {
    CallBase* epoch = getAliasEpochs(group)[idx];
    invalidateFunctionAnalyses(epoch->getFunction());
    epoch->eraseFromParent();
}

export void removeFromAliasGroup(int group, int idx) {
    Value* V = *std::next(aliasInfo[group].members.begin(), idx);
    aliasInfo[group].members.erase(V);
    for (User* U : curM->getFunction("CoVer_AnnotAlias")->users()) {
        if (CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledOperand()->getName() != "CoVer_AnnotAlias") continue;
            if (CB->getArgOperand(0) != V) continue;
            if (dyn_cast<ConstantInt>(CB->getArgOperand(2))->getZExtValue() != group) continue;
            invalidateFunctionAnalyses(CB->getFunction());
            CB->eraseFromParent();
            break;
        }
    }
    if (aliasInfo[group].members.empty()) {
        // Group is gone, delete its epochs
        for (CallBase* CB : getAliasEpochs(group)) {
            invalidateFunctionAnalyses(CB->getFunction());
            CB->eraseFromParent();
        }
        aliasInfo.erase(group);
    }
}

export void addAliasEpoch(int group, Instruction* before) {
    if (isa<PHINode>(before)) before = &*before->getParent()->getFirstInsertionPt();
    CallInst* CI = CallInst::Create(curM->getFunction("CoVer_AnnotAliasEpoch"), {getInt(group)});
    invalidateFunctionAnalyses(before->getFunction());
    CI->insertBefore(before->getIterator());
    CI->setDebugLoc(before->getDebugLoc());
}

export void setFPTarget(CallBase* indirect, std::set<Function*> targets) {
    assert(indirect->isIndirectCall());

    // Replace the current annotation. The new one is placed directly before the call, where the called value is available
    CallBase* old = getFPAnnotCall(indirect);
    invalidateFunctionAnalyses(indirect->getFunction());
    if (old) old->eraseFromParent();
    if (targets.empty()) return;

    std::vector<Value*> args = {indirect->getCalledOperand(), getInt(targets.size())};
    args.insert(args.end(), targets.begin(), targets.end());
    CallInst* CI = CallInst::Create(indirect->getModule()->getFunction("CoVer_AnnotFP"), args);
    CI->insertBefore(indirect->getIterator());
    CI->setDebugLoc(indirect->getDebugLoc());
}

// Annotate the pointer produced by I as allocated with at least size bytes (0: size unknown). Returns error message, empty on success
export std::string setAllocAnnot(Instruction* I, std::uint64_t size) {
    if (!I->getType()->isPointerTy()) return "Instruction does not produce a pointer value!";
    Instruction* insertPt;
    if (InvokeInst* II = dyn_cast<InvokeInst>(I)) {
        // Result only available in normal destination
        if (!II->getNormalDest()->getSinglePredecessor()) return "Normal destination of invoke has multiple predecessors, cannot place annotation!";
        insertPt = &*II->getNormalDest()->getFirstInsertionPt();
    } else if (I->isTerminator()) {
        return "Cannot place annotation after a terminator instruction!";
    } else if (isa<PHINode>(I)) {
        insertPt = &*I->getParent()->getFirstInsertionPt();
    } else {
        insertPt = I->getNextNode();
    }

    ConstantInt* sizeConst = ConstantInt::get(Type::getInt64Ty(curM->getContext()), size);
    // Already annotated: Only update size
    for (CallBase* CB = dyn_cast<CallBase>(insertPt); CB && CB->getCalledOperand()->getName() == "CoVer_AnnotAlloc"; CB = dyn_cast_or_null<CallBase>(CB->getNextNode())) {
        if (CB->getArgOperand(0) != I) continue;
        CB->setArgOperand(1, sizeConst);
        return "";
    }
    CallInst* CI = CallInst::Create(curM->getFunction("CoVer_AnnotAlloc"), {I, sizeConst});
    CI->insertBefore(insertPt->getIterator());
    CI->setDebugLoc(I->getDebugLoc());
    return "";
}

}
