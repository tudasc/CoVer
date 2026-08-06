module;

#include "ContractTree.hpp"
#include <climits>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <optional>
#include <sys/types.h>
#include <stack>
#include <utility>
#include <vector>

#include "dsa/DSNode.h"
#include "dsa/DSSupport.h"
#include "dsa/Steensgaard.hh"
#include "dsa/DSGraph.h"

export module ContractPassUtility;
import LLVMModule;
import ErrorMessage;
import BasicTypes;

using namespace llvm;

// The module to analyse
Module* curM;

// Basic Types
BasicTypesAnalysis::BasicTypes Basic_Types;

// To only warn once if a CB is calling an unknown function
static std::set<const CallBase*> UnknownCalledParam;

// For language-specific stuff
static bool isFort = false;

// Map from function to indirect calls to it, as gotten by annotations
std::map<const Function*, std::set<CallBase*>> AnnotFuncReverse;

// Whether to output IR or human-readable
bool outputIR = false;

template<typename T>
struct WorklistEntry {
    Instruction* start;
    T initial;
    std::stack<CallBase*> stack;
};

namespace ContractPassUtility {

export template<typename T>
using TransferFunction = std::function<T(T,const Instruction*,void*)>;
export template<typename T>
using MergeFunction = std::function<std::pair<T,bool>(T,T,const Instruction*,void*)>;

export enum struct TraceKind { LINEAR, BRANCH, SWITCH, FUNCENTRY, FUNCEXIT };
export template<typename T>
struct JumpTraceEntry {
    JumpTraceEntry<T>() {}
    JumpTraceEntry<T>(T ai, Instruction* I, TraceKind k, std::vector<JumpTraceEntry<T>*> preds) : analysisInfo{ai}, loc{I}, kind{k}, predecessors{preds} {}
    T analysisInfo;
    Instruction* loc;
    TraceKind kind;
    std::vector<JumpTraceEntry<T>*> predecessors;
    bool operator<(const JumpTraceEntry<T> other) const { return loc < other.loc; };
};
export template<typename T>
struct TraceDB : std::shared_ptr<DenseMap<Instruction*, std::unique_ptr<JumpTraceEntry<T>>>> {
    TraceDB<T>() : std::shared_ptr<DenseMap<Instruction*, std::unique_ptr<JumpTraceEntry<T>>>>(std::make_shared<DenseMap<Instruction*, std::unique_ptr<JumpTraceEntry<T>>>>()) {}
    JumpTraceEntry<T>* operator[](Instruction* I) const {
        std::unique_ptr<JumpTraceEntry<T>>& entry = (**this)[I];
        if (!entry) entry = std::make_unique<JumpTraceEntry<T>>();
        return entry.get();
    }
};
export template<typename T>
struct WorklistResult : GenericWLRes {
    DenseMap<Instruction*, T> AnalysisInfo;
    TraceDB<T> JumpTraces;
    std::function<std::string(T)> AnalysisInfoToStr = nullptr;
};

export struct AliasGroup {
    std::set<Value*, std::less<>> members;
    bool areAliasing;
};

// Annotation infos
std::map<int, ContractPassUtility::AliasGroup> aliasInfo;

#define DEBUG_ENV "COVER_LLVM_DEBUG"
export bool isDebug() {
    return getenv(DEBUG_ENV) != NULL && atoi(getenv(DEBUG_ENV)) == 1;
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
export std::set<Function*> getFPAnnots(CallBase const* CB) {
    if (!CB->isIndirectCall()) return {};

    std::set<Function*> fp_targets;
    Instruction const* cur = CB->getPrevNode();
    for (int i = 0; i < 5 && cur; i++) {
        if (CallBase const* annotCall = dyn_cast<CallBase>(cur)) {
            if (annotCall->getCalledOperand()->getName() == "CoVer_AnnotFP" && sameOriginCheck(annotCall->getArgOperand(0), CB->getCalledOperand())) {
                for (int i = 2; i < annotCall->arg_size(); i++) {
                    fp_targets.insert(CB->getModule()->getFunction(annotCall->getArgOperand(i)->getName()));
                }
                break;
            }
        }
        cur = cur->getPrevNode();
    }
    return fp_targets;
}

template<typename T>
void updateJumpTrace(ContractPassUtility::TraceDB<T> const& trace, Instruction* cur, Instruction* prev, ContractPassUtility::TraceKind kind, T analysisInfo) {
    if (trace->contains(cur)) {
        if (std::find(trace[cur]->predecessors.begin(), trace[cur]->predecessors.end(), trace[prev]) == trace[cur]->predecessors.end())
            trace[cur]->predecessors.push_back(trace[prev]);
        trace[cur]->analysisInfo = analysisInfo;
    } else {
        std::vector<ContractPassUtility::JumpTraceEntry<T>*> preds;
        if (trace->contains(prev)) preds.push_back(trace[prev]);
        *trace[cur] = ContractPassUtility::JumpTraceEntry<T>(analysisInfo, cur, kind, preds);
    }
}

template <typename T>
std::pair<T, bool> getMergeResult(DenseMap<Instruction*, T>& AI, ContractPassUtility::TraceDB<T> const& trace, T prevInfo, ContractPassUtility::MergeFunction<T> const& merge, Instruction* cur, void* data) {
    bool resume = true;
    T info;
    auto it = AI.find(cur);
    if (it == AI.end()) {
        info = prevInfo;
    } else {
        std::tie(info, resume) = merge(prevInfo, it->second, cur, data);
    }
    AI[cur] = info;
    if (trace->contains(cur)) trace[cur]->analysisInfo = info;
    return {info, resume};
}

/*
 * Apply worklist algorithm
 * Need Start param to make sure that the initialization of parameters does not count as operation
 */
export template <typename T>
ContractPassUtility::WorklistResult<T> GenericWorklist(Instruction* Start, bool transferFirst, TransferFunction<T> transfer, MergeFunction<T> merge, void* data, T init) {
    // Jumptrace
    TraceDB<T> jumptraces;
    // Analysis Info mapping
    DenseMap<Instruction*, T> postAccess;
    // Worklist
    std::queue<WorklistEntry<T>> todoList;

    if (!transferFirst) {
        updateJumpTrace(jumptraces, Start, nullptr, TraceKind::LINEAR, init);
        todoList.push({Start->getNextNode(), init, {}});
        updateJumpTrace(jumptraces, Start->getNextNode(), Start, TraceKind::LINEAR, init);
    } else {
        todoList.push({Start, init, {}});
        updateJumpTrace(jumptraces, Start, nullptr, TraceKind::LINEAR, init);
    }

    // Map of OpenMP functions to index with function pointer
    static const std::map<StringRef,int> OMPNames = {{"__kmpc_omp_task_alloc", 5}, {"__kmpc_fork_call", 2}};

    // Start of worklist algorithm
    while (!todoList.empty()) {
        Instruction* cur = todoList.front().start;
        T prevInfo = todoList.front().initial;
        std::stack<CallBase*> stack = todoList.front().stack;

        while (cur != nullptr) {
            // Add previous info depending on following conditions:
            // 1. In any case, prevInfo MUST have the corresponding access
            // 2. Either: It is a "may" analysis, "next" was until now unreachable, or it was reached before and already contains the access
            // If those apply, add that access. Otherwise, remove it if present (=> "must" analysis, node reached with info, jumped here without)
            std::pair<T, bool> mergeRes = getMergeResult(postAccess, jumptraces, prevInfo, merge, cur, data);
            if (!mergeRes.second) {
                // Already visited and analysis does not wish to pursue further.
                // Remove from worklist, or pop stack
                Instruction* tmpNext = nullptr;
                while (!tmpNext && !stack.empty()) {
                    tmpNext = stack.top()->getNextNode();
                    stack.pop();
                }
                // Either tmpNext is set, or null because tail-call stack end or stack was empty
                if (tmpNext) {
                    cur = tmpNext;
                } else break;
            }

            // Call transfer function
            postAccess[cur] = transfer(postAccess[cur], cur, data);
            prevInfo = postAccess[cur];

            // Check for branching / terminating instructions
            // Missing because not sure if needed / relevant / used / too little info / lazy:
            // CleanupReturnInst, CatchReturnInst, CatchSwitchInst, CallBrInst, ResumeInst, InvokeInst, IndirectBrInst
            if (BranchInst* BR = dyn_cast<BranchInst>(cur)) {
                for (BasicBlock* alt : BR->successors()) {
                    updateJumpTrace(jumptraces, &alt->front(), cur, TraceKind::BRANCH, postAccess[cur]);
                    todoList.push( {&alt->front(), postAccess[cur], stack} );
                }
                break;
            }
            if (SwitchInst* SI = dyn_cast<SwitchInst>(cur)) {
                for (unsigned i = 0; i < SI->getNumSuccessors(); i++) {
                    BasicBlock* alt = SI->getSuccessor(i);
                    updateJumpTrace(jumptraces, &alt->front(), cur, TraceKind::SWITCH, postAccess[cur]);
                    todoList.push( {&alt->front(), postAccess[cur], stack} );
                }
                break;
            }
            if (isa<UnreachableInst>(cur)) {
                break;
            }

            // Check if function call: If it is, jump to function body
            // If not, continue with normal next instruction
            Instruction* next = nullptr;
            TraceKind next_trace_entry;
            if (CallBase* CB = dyn_cast<CallBase>(cur)) {
                if (CB->getCalledFunction() && (OMPNames.contains(CB->getCalledFunction()->getName()) || !CB->getCalledFunction()->isDeclaration())) {
                    stack.push(CB);
                    if (!OMPNames.contains(CB->getCalledFunction()->getName())) {
                        next = &CB->getCalledFunction()->getEntryBlock().front();
                        next_trace_entry = TraceKind::FUNCENTRY;
                    } else {
                        if (Function* ompFunc = dyn_cast<Function>(CB->getArgOperand(OMPNames.at(CB->getCalledFunction()->getName())))) {
                            if (!ompFunc->isDeclaration())
                                next = &ompFunc->getEntryBlock().front();
                        }
                        if (!next) {
                            errs() << "NOTE: Could not resolve OpenMP outlined call! Verification accuracy is impaired\n";
                            stack.pop();
                        }
                    }
                } else {
                    // Check for annotations
                    std::set<Function*> annots = getFPAnnots(CB);
                    bool foundnext = false;
                    for (Function* F : annots) {
                        if (F->isDeclaration()) continue;
                        std::stack<CallBase*> new_stack = stack;
                        new_stack.push(CB);
                        AnnotFuncReverse[F].insert(CB);
                        updateJumpTrace(jumptraces, &F->getEntryBlock().front(), cur, TraceKind::FUNCENTRY, postAccess[cur]);
                        todoList.push( {&F->getEntryBlock().front(), postAccess[cur], new_stack} );
                        foundnext = true;
                    }
                    if (foundnext) goto next_iter;
                }
            }
            if (!next && !isa<ReturnInst>(cur)) {
                next = cur->getNextNode();
                if (next) next_trace_entry = TraceKind::LINEAR;
            }

            // Check if returning from function
            if (isa<ReturnInst>(cur) && !stack.empty()) {
                // Forward to next from stack
                next = stack.top()->getNextNode();
                stack.pop();
                next_trace_entry = TraceKind::FUNCEXIT;
            } else if (isa<ReturnInst>(cur)) {
                // Stack is empty. But if we started inside a function, context includes all callsites
                Function* func = cur->getFunction();
                for (User* U : func->users()) {
                    if (CallBase* CB = dyn_cast<CallBase>(U)) {
                        if (CB->getCalledOperand() != func) continue;
                        // Add callsite next to todoList
                        todoList.push( {CB->getNextNode(), postAccess[cur], stack} );
                    }
                }
            }

            // Update traces
            if (next) {
                updateJumpTrace(jumptraces, next, cur, next_trace_entry, postAccess[cur]);
            }

            // Know next instruction, continue loop or iter is null and we are done
            cur = next;
        }
        next_iter:
        todoList.pop();
    }
    return {{}, postAccess, jumptraces, nullptr};
}

Function const* getParentFunction(Value const* V) {
    if (Instruction const* I = dyn_cast<Instruction>(V))
        return I->getFunction();
    if (Argument const* Arg = dyn_cast<Argument>(V))
        return Arg->getParent();
    return nullptr;
}

std::set<int> getAliasGroups(Value const* V) {
    std::set<int> res;
    if (Instruction const* I = dyn_cast<Instruction>(V)) {
        Instruction const* cur = I->getNextNode();
        for (int i = 0; i < 5; i++) {
            if (CallBase const* CB = dyn_cast<CallBase>(cur)) {
                if (CB->getCalledOperand()->getName() == "CoVer_AnnotAlias") {
                    if (sameOriginCheck(CB->getArgOperand(0), V)) res.insert(dyn_cast<ConstantInt>(CB->getArgOperand(2))->getSExtValue());
                }
            }
            if (!cur->getNextNode()) break;
            cur = cur->getNextNode();
        }
    } else {
        Function* F = curM->getFunction("main");
        for (Instruction& I : instructions(F)) {
            if (isa<AllocaInst>(&I)) continue;
            if (CallBase* CB = dyn_cast<CallBase>(&I)) {
                if (CB->getCalledOperand()->getName() == "CoVer_AnnotAlias") {
                    if (CB->getArgOperand(0) == V) res.insert(dyn_cast<ConstantInt>(CB->getArgOperand(2))->getSExtValue());
                }
            } else break;
        }
    }
    return res;
}

std::set<std::pair<const Value*, int>> resolveArgForFuncDiff(Value const* I, int curSteps, std::map<const Value*,int> candidatesConsidered) {
    // First step: Collect the arguments represented by the value
    std::set<Argument const*> args;
    if (const Argument* Arg = dyn_cast<Argument>(I)) {
        args.insert(Arg);
    }
    if (const AllocaInst* AI = dyn_cast<AllocaInst>(I)) {
        // Pre-sroa: Possibly a function parameter, check args against storeinst users
        const Function* tmp = AI->getFunction();
        for (const User* UA : AI->users() ) {
            if (!isa<StoreInst>(UA)) continue;
            Value const* potentialArg = dyn_cast<StoreInst>(UA)->getValueOperand();
            if (Argument const* Arg = dyn_cast<Argument>(potentialArg)) {
                args.insert(Arg);
                curSteps--;
                break;
            }
        }
    }

    // Second step: extract callsites and add to candidate list
    std::set<std::pair<const Value*, int>> candidates;
    for (Argument const* Arg : args) {
        Function const* F = Arg->getParent(); 
        for (const User* U : F->users()) {
            if (const CallBase* CB = dyn_cast<CallBase>(U)) {
                if (CB->getCalledOperand() != F) continue;
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

std::map<const Value*,int> getFunctionParentInstrCandidates(const Value* Ip) {
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

export StoreInst* getLastStore(CallBase* CB, int idx, FunctionAnalysisManager* FAM) {
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
    Basic_Types = MAM.getResult<BasicTypesAnalysis>(M);

    isFort = M.getFunction("_QQmain");
    curM = &M;

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
}

export std::optional<uint> getLineNumber(const Instruction* I) {
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
            if (CB->getCalledOperand()->getName() == Target ||  // C-style match
                CB->getCalledOperand()->getName() == Target.lower() + "_") { // Fortran-style match
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

    // Check for alias annotations
    // First, check if both are in alias group
    if (source && target) {
        for (std::pair<int, AliasGroup> AG : aliasInfo) {
            if ((AG.second.members.contains(source) || AG.second.members.contains(betterGetPointerOperand(source)))
            && (AG.second.members.contains(target) || AG.second.members.contains(betterGetPointerOperand(target)))) {
                return AG.second.areAliasing;
            }
        }
        // User annotations are a bit later - Check those now
        std::set<int> aliasTgt = getAliasGroups(target), aliasSrc = getAliasGroups(source);
        std::vector<int> shared;
        std::set_intersection(aliasSrc.begin(), aliasSrc.end(), aliasTgt.begin(), aliasTgt.end(), std::back_inserter(shared));
        if (!shared.empty()) return getAliasAnnots().at(shared.front()).areAliasing;
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
            errs() << "Could not find candidate parameter with matching tag! Invalid contract definition!\n";
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
            // Try instructions first
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
    CallInst* CI = CallInst::Create(curM->getFunction("CoVer_AnnotAlias"), {V, Basic_Types.getBool(aliasInfo[idx].areAliasing), Basic_Types.getInt(idx)});
    if (Instruction* I = dyn_cast<Instruction>(V)) {
        CI->insertAfter(I);
        CI->setDebugLoc(I->getDebugLoc());
    } else {
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
            CB->setArgOperand(1, Basic_Types.getBool(isAlias));
        }
    }
}
export void removeFromAliasGroup(int group, int idx) {
    Value* V = *std::next(aliasInfo[group].members.begin(), idx);
    aliasInfo[group].members.erase(V);
    for (User* U : curM->getFunction("CoVer_AnnotAlias")->users()) {
        if (CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledOperand()->getName() != "CoVer_AnnotAlias") continue;
            if (CB->getArgOperand(0) != V) continue;
            if (dyn_cast<ConstantInt>(CB->getArgOperand(2))->getZExtValue() != idx) continue;
            CB->eraseFromParent();
            break;
        }
    }
    if (aliasInfo[group].members.empty()) aliasInfo.erase(group);
}

export void setFPTarget(CallBase* indirect, std::set<Function*> targets) {
    assert(indirect->isIndirectCall());

    std::vector<Value*> args = {indirect->getCalledOperand(), Basic_Types.getInt(targets.size())};
    args.insert(args.end(), targets.begin(), targets.end());
    CallInst* CI = CallInst::Create(indirect->getModule()->getFunction("CoVer_AnnotFP"), args);
    if (indirect->getPrevNode() && isa<CallBase>(indirect->getPrevNode()) && dyn_cast<CallBase>(indirect->getPrevNode())->getCalledOperand()->getName() == "CoVer_AnnotFP") {
        ReplaceInstWithInst(indirect->getPrevNode(), CI);
    } else {
        CI->insertBefore(indirect->getIterator());
    }
    if (targets.empty()) indirect->getPrevNode()->eraseFromParent();
    else indirect->getPrevNode()->setDebugLoc(indirect->getDebugLoc());
}

}
