module;

#include <cassert>
#include <climits>
#include <cstdlib>

export module ContractPassUtility;
import std;
import LLVMModule;
import ErrorMessage;
import ValueOrder;

using namespace llvm;

// The module to analyse
Module* curM;

// 32 bit integer constant, the type of the int parameters of CoVer's annotation functions
ConstantInt* getInt(int x) { return ConstantInt::get(Type::getInt32Ty(curM->getContext()), x); }

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

// Analyses of the current module
FunctionAnalysisManager* curFAM = nullptr;
ModuleAnalysisManager* curMAM = nullptr;

export ModuleAnalysisManager& getModuleAnalyses() { return *curMAM; }

// Whether the module is a Fortran program, which needs language-specific handling
export bool isFortran() { return isFort; }

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

export std::map<int, AliasGroup> const& getAliasAnnots() { return aliasInfo; }
// The memory state a load reads: The last access that may have written the loaded memory
MemoryAccess const* getReachingDef(LoadInst const* LI) {
    if (!curFAM) return nullptr;
    MemorySSA& MSSA = curFAM->getResult<MemorySSAAnalysis>(*const_cast<Function*>(LI->getFunction())).getMSSA();
    return MSSA.getWalker()->getClobberingMemoryAccess(LI);
}

class SlotTrackerUtil {
    private:
        std::unique_ptr<ModuleSlotTracker> tracker;
    public:
        ModuleSlotTracker& getSlotTracker(Function const& F) {
            if (!tracker) tracker = std::make_unique<ModuleSlotTracker>(curM, true);
            tracker->incorporateFunction(F);
            return *tracker;
        }
        void reset() { tracker.reset(); }
};

SlotTrackerUtil slotTracker;

// IR edits invalidate the memory SSA and the value numbering of the edited function
export void invalidateFunctionAnalyses(Function* F) {
    slotTracker.reset();
    if (curFAM && F) curFAM->invalidate(*F, PreservedAnalyses::none());
}

// Name of a value as written in the IR and accepted by getValueByName, e.g. %42 or %buf. Globals and constants without %
export std::string getValueName(Value const* V) {
    Function const* F = nullptr;
    if (Instruction const* I = dyn_cast<Instruction>(V)) F = I->getFunction();
    else if (Argument const* A = dyn_cast<Argument>(V)) F = A->getParent();
    if (!F) return V->getNameOrAsOperand();
    if (V->hasName()) return "%" + V->getName().str();
    int slot = slotTracker.getSlotTracker(*F).getLocalSlot(V);
    return slot >= 0 ? std::format("%{}", slot) : V->getNameOrAsOperand();
}

// The value as printed in the IR, numbered with the shared tracker so that printing many values of a function stays linear
export std::string printValue(Value const* V) {
    std::string out;
    raw_string_ostream os(out);
    Function const* F = nullptr;
    if (Instruction const* I = dyn_cast<Instruction>(V)) F = I->getFunction();
    else if (Argument const* A = dyn_cast<Argument>(V)) F = A->getParent();
    if (F) V->print(os, slotTracker.getSlotTracker(*F));
    else V->print(os);
    return out;
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
        fp_targets.insert(dyn_cast<Function>(annot->getArgOperand(i)->stripPointerCastsAndAliases()));
    }
    return fp_targets;
}

// The indirect calls that may call F according to their function pointer annotations
export std::set<CallBase*> const& getAnnotatedIndirectCalls(Function const* F) {
    static const std::set<CallBase*> none;
    std::map<const Function*, std::set<CallBase*>>::const_iterator it = AnnotFuncReverse.find(F);
    return it != AnnotFuncReverse.end() ? it->second : none;
}

// Called function, also if called through an alias (e.g. C++ complete object constructor aliasing the base object one)
export Function* getCalleeThroughAlias(CallBase const* CB) {
    return dyn_cast<Function>(CB->getCalledOperand()->stripPointerCastsAndAliases());
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
    ValueOrder::determinize(*curM);

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

    curFAM = &MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    curMAM = &MAM;

    outputIR = _outputIR;

    // Function list and basic blocks must be sorted for clean diff on interactive analysis
    determinizeModule();
    slotTracker.reset(); // Numbering follows the new order

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
                llvm::raw_string_ostream os(it->second);
                I->print(os, slotTracker.getSlotTracker(*I->getFunction()));
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

export Value* getValueByName(std::string name, Function* F) {
    if (name.empty()) return nullptr;
    if (name.starts_with("%")) {
        if (!F || F->isDeclaration()) return nullptr;
        std::string_view local = std::string_view(name).substr(1);
        // Unnamed values by their number. Getting it from the shared tracker instead of printing each value keeps this linear
        if (std::optional<int> slot = parseInt(local); slot && *slot >= 0) {
            ModuleSlotTracker& MST = slotTracker.getSlotTracker(*F);
            for (Argument& A : F->args()) {
                if (!A.hasName() && MST.getLocalSlot(&A) == *slot) return &A;
            }
            for (Instruction& I : instructions(*F)) {
                if (!I.hasName() && MST.getLocalSlot(&I) == *slot) return &I;
            }
        }
        // Named values
        for (Argument& A : F->args()) {
            if (A.getName() == StringRef(local)) return &A;
        }
        for (Instruction& I : instructions(*F)) {
            if (I.getName() == StringRef(local)) return &I;
        }
        return nullptr;
    }
    return F->getParent()->getNamedValue(name.substr(1));
}

constexpr std::string_view REMOVED_ANNOT_FUNC = "CoVer_AnnotRemoved";

void removeAnnot(CallBase* annot) {
    FunctionType* removedType = FunctionType::get(Type::getVoidTy(curM->getContext()), true);
    annot->setCalledFunction(curM->getOrInsertFunction(REMOVED_ANNOT_FUNC, removedType));
}

export bool isRemovedAnnot(Instruction const* I) {
    CallBase const* CB = dyn_cast<CallBase>(I);
    return CB && CB->getCalledOperand()->getName().str() == REMOVED_ANNOT_FUNC;
}

// Prints the module as if the removed annotations were already erased
export void printWithoutRemovedAnnots(Module& M, raw_ostream& out) {
    Function* removedFunc = M.getFunction(REMOVED_ANNOT_FUNC);
    if (!removedFunc) return M.print(out, nullptr);
    // Detach the removed annotations and the function for printing. Annotations are no terminators, so each has a next instruction to be reinserted before
    std::vector<std::pair<Instruction*, Instruction*>> detached;
    for (User* U : removedFunc->users()) {
        Instruction* annot = dyn_cast<Instruction>(U);
        if (!annot || !annot->getParent()) continue;
        detached.push_back({annot, annot->getNextNode()});
        annot->removeFromParent();
    }
    removedFunc->removeFromParent();
    M.print(out, nullptr);
    M.getFunctionList().push_back(removedFunc);
    // Reverse order, so that the next instruction of each annotation is in place again when reinserting it
    for (auto const& [annot, next] : std::views::reverse(detached)) annot->insertBefore(next->getIterator());
}

// Erases the removed annotations. Only valid once no trace refers to them anymore
export void eraseRemovedAnnots(Module& M) {
    Function* removedFunc = M.getFunction(REMOVED_ANNOT_FUNC);
    if (!removedFunc) return;
    slotTracker.reset(); // Must not keep erased instructions
    for (User* U : make_early_inc_range(removedFunc->users())) {
        if (Instruction* annot = dyn_cast<Instruction>(U)) annot->eraseFromParent();
    }
    removedFunc->eraseFromParent();
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
export int createAliasGroup(bool shouldAlias, Value* V1, Value* V2) {
    int group = 0;
    while (aliasInfo.contains(group)) group++;
    aliasInfo[group] = {{}, shouldAlias};
    for (Value* V : {V1, V2}) {
        addToAliasGroup(group, V);
    }
    return group;
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
    removeAnnot(getAliasEpochs(group)[idx]);
}

export void removeFromAliasGroup(int group, int idx) {
    Value* V = *std::next(aliasInfo[group].members.begin(), idx);
    aliasInfo[group].members.erase(V);
    for (User* U : curM->getFunction("CoVer_AnnotAlias")->users()) {
        if (CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledOperand()->getName() != "CoVer_AnnotAlias") continue;
            if (CB->getArgOperand(0) != V) continue;
            if (dyn_cast<ConstantInt>(CB->getArgOperand(2))->getZExtValue() != group) continue;
            removeAnnot(CB);
            break;
        }
    }
    if (aliasInfo[group].members.empty()) {
        // Group is gone, delete its epochs
        for (CallBase* CB : getAliasEpochs(group)) removeAnnot(CB);
        aliasInfo.erase(group);
    }
}

export void addAliasEpoch(int group, Instruction* before) {
    if (isa<PHINode>(before)) before = &*before->getParent()->getFirstInsertionPt();
    CallInst* CI = CallInst::Create(curM->getFunction("CoVer_AnnotAliasEpoch"), {getInt(group)});
    invalidateFunctionAnalyses(before->getFunction());
    CI->insertBefore(before->getIterator());
    CI->setDebugLoc(before->getDebugLoc());
    // E.g. before the allocas at the start of a function: Located at the function, so that it can be told where the epoch is
    DISubprogram* SP = before->getFunction()->getSubprogram();
    if (!CI->getDebugLoc() && SP) CI->setDebugLoc(DILocation::get(curM->getContext(), SP->getScopeLine() ? SP->getScopeLine() : SP->getLine(), 0, SP));
}

export void setFPTarget(CallBase* indirect, std::set<Function*> targets) {
    assert(indirect->isIndirectCall());

    // Replace the current annotation. The new one is placed directly before the call, where the called value is available
    CallBase* old = getFPAnnotCall(indirect);
    invalidateFunctionAnalyses(indirect->getFunction());
    if (old) removeAnnot(old);
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

    invalidateFunctionAnalyses(I->getFunction());
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
