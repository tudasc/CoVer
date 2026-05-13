#include "ContractPassUtility.hpp"
#include "ContractTree.hpp"
#include "ErrorMessage.h"
#include "../Passes/BasicTypes.hpp"
#include <climits>
#include <iterator>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryDependenceAnalysis.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugProgramInstruction.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/WithColor.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <optional>
#include <sys/types.h>
#include <utility>
#include <vector>

#include "dsa/DSNode.h"
#include "dsa/DSSupport.h"
#include "dsa/Steensgaard.hh"
#include "dsa/DSGraph.h"

using namespace llvm;

// The module to analyse
Module* curM;

// Basic Types
BasicTypesAnalysis::BasicTypes Basic_Types;

// To only warn once if a CB is calling an unknown function
static std::set<const CallBase*> UnknownCalledParam;

// For language-specific stuff
static bool isFort = false;

// Annotation infos
std::map<int, ContractPassUtility::AliasGroup> aliasInfo;

const Value* ContractPassUtility::betterGetPointerOperand(const Value* V) {
    const Value* b = getPointerOperand(V);
    if (b == nullptr) {
        if (const GEPOperator* GEPOp = dyn_cast<GEPOperator>(V)) b = GEPOp->getPointerOperand();
    }
    return b;
}
Function const* getParentFunction(Value const* V) {
    if (Instruction const* I = dyn_cast<Instruction>(V))
        return I->getFunction();
    if (Argument const* Arg = dyn_cast<Argument>(V))
        return Arg->getParent();
    return nullptr;
}

// Map from function to indirect calls to it, as gotten by annotations
std::map<const Function*, std::set<CallBase*>> AnnotFuncReverse;

StoreInst* ContractPassUtility::getLastStore(CallBase* CB, int idx, FunctionAnalysisManager* FAM) {
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

namespace ContractPassUtility {

std::map<int, AliasGroup> const getAliasAnnots() { return aliasInfo; }
std::set<Function*> const getFPAnnots(CallBase* CB) {
    if (!CB->isIndirectCall()) return {};

    std::set<Function*> fp_targets;
    if (CB->getPrevNode() && isa<CallBase>(CB->getPrevNode())) {
        CallBase* annotCall = dyn_cast<CallBase>(CB->getPrevNode());
        if (annotCall->getCalledOperand()->getName() == "CoVer_AnnotFP") {
            for (int i = 2; i < annotCall->arg_size(); i++) {
                fp_targets.insert(CB->getModule()->getFunction(annotCall->getArgOperand(i)->getName()));
            }
        }
    }
    return fp_targets;
}

void Initialize(Module& M, ModuleAnalysisManager& MAM) {
    Basic_Types = MAM.getResult<BasicTypesAnalysis>(M);

    isFort = M.getFunction("_QQmain");
    curM = &M;

    FunctionType* AnnotAliasT = FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Ptr_Type, Basic_Types.Bool_Type, Basic_Types.Int_Type}, false);
    FunctionCallee AnnotAliasCallee = M.getOrInsertFunction("CoVer_AnnotAlias", AnnotAliasT);
    for (User* U : AnnotAliasCallee.getCallee()->users()) {
        if (CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledOperand() == AnnotAliasCallee.getCallee()) {
                int groupIdx = dyn_cast<ConstantInt>(CB->getArgOperand(2))->getZExtValue();
                if (!aliasInfo.contains(groupIdx)) aliasInfo[groupIdx];
                aliasInfo[groupIdx].areAliasing = dyn_cast<ConstantInt>(CB->getArgOperand(1))->getZExtValue();
                #warning TODO need to get actual value here maybe
                aliasInfo[groupIdx].members.insert(CB->getArgOperand(0));
            }
        }
    }
}

std::optional<uint> getLineNumber(const Instruction* I) {
    if (const DebugLoc& N = I->getDebugLoc()) {
        return N.getLine();
    }
    return std::nullopt;
}
std::string getFile(const Instruction* I, bool longform = true) {
    if (I->getDebugLoc())
        return (I->getDebugLoc()->getDirectory() != "" && longform ? I->getDebugLoc()->getDirectory() + "/" : "").str() + I->getDebugLoc()->getFilename().str();
    return "UNKNOWN";
}

std::string getInstrLocStr(const Function* I, bool longform) {
    if (const DISubprogram* debugLoc = I->getSubprogram())
        return ((longform ? debugLoc->getDirectory() + "/" : "") + debugLoc->getFilename()).str() + ":" + std::to_string(debugLoc->getLine());
    return "UNKNOWN";
}

std::string getInstrLocStr(const Instruction* I, bool longform) {
    if (const DebugLoc &debugLoc = I->getDebugLoc())
        return getFile(I, longform) + ":" + std::to_string(debugLoc.getLine()) + (longform ? ":" + std::to_string(debugLoc->getColumn()) : "");
    return "UNKNOWN";
}

FileReference getFileReference(const Instruction* I) {
    return {
        .file = getFile(I),
        .line = I->getDebugLoc() ? I->getDebugLoc()->getLine() : 0,
        .column = I->getDebugLoc() ? I->getDebugLoc()->getColumn() : 0
    };
}

bool isTrivialAlloc(Value const* V) {
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

ConstantInt* fortCheckAndGetGlbInt(Value* V) {
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

bool checkCalledApplies(const CallBase* CB, const StringRef Target, bool isTag, std::map<Function*, std::vector<ContractTree::TagUnit>> Tags) {
    if (!isTag) {
        if (CB->getCalledOperand()->getName().empty()) {
            if (!UnknownCalledParam.contains(CB)) {
                errs() << "Could not get name for function at " << getInstrLocStr(CB) << "!\nAnalysis performance is impaired!\n";
                UnknownCalledParam.insert(CB);
            }
            return false;
        }
        return CB->getCalledOperand()->getName() == Target ||  // C-style match
               CB->getCalledOperand()->getName() == Target.lower() + "_"; // Fortran-style match
    } else {
        Function* F = (Function*)CB->getCalledOperand(); // Dirty cast ok, no member access. Needed because of fortran non-matching param
        if (!Tags.contains(F)) return false;
        for (const ContractTree::TagUnit tag : Tags[F]) {
            if (tag.tag == Target) {
                return true;
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

std::vector<std::string> getCoVerAnnotations(Instruction* I) {
    std::vector<std::string> cover_annots;
    if (MDNode* Existing = I->getMetadata(LLVMContext::MD_annotation)) {
        MDTuple* Tuple = cast<MDTuple>(Existing);
        for (MDOperand const& N : Tuple->operands()) {
            if (isa<MDString>(N.get())) {
                std::string annot = cast<MDString>(N.get())->getString().str();
                if (annot.starts_with("CoVer_Annot")) cover_annots.push_back(annot);
            }
        }
    }
    return cover_annots;
}

bool checkParamMatch(const Value* contrP, const Value* callP, ContractTree::ParamAccess acc, ModuleAnalysisManager* MAM) {
    const Value* source = contrP;
    const Value* target = callP;
    int diff = 0;

    // Filter out new instr from sroa
    if (isa<StoreInst>(callP) && dyn_cast<StoreInst>(callP)->getPointerOperand()->getName().starts_with(".fca.") ||
        isa<StoreInst>(contrP) && dyn_cast<StoreInst>(contrP)->getPointerOperand()->getName().starts_with(".fca.")) {
        return false;
    }

    // Annotated infos
    for (std::pair<int, AliasGroup> AG : aliasInfo) {
        if (AG.second.members.contains(source) && AG.second.members.contains(target)) {
            return AG.second.areAliasing;
        }
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
                if (!V && IS_DEBUG) WithColor(errs(), HighlightColor::String) << "Note: Static deref failed, falling back to orig.\n";
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
        while (true) {
            // If either is null, paramerror or if one does not have a pointer operand, then they can not match
            if (!source || !target) return false;
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
        }
    }
    return false;
}

bool checkCallParamApplies(const CallBase* Source, const CallBase* Target, const std::string TargetStr, ContractTree::CallParam const& P, std::map<Function*, std::vector<ContractTree::TagUnit>> Tags, ModuleAnalysisManager* MAM) {
    std::vector<const Value*> candidateParams;
    const Value* sourceParam = Source->getArgOperand(P.contrP);

    if (!P.callPisTagVar) {
        candidateParams.push_back(Target->getArgOperand(P.callP));
    } else {
        for (ContractTree::TagUnit TagU : Tags[(Function*)Target->getCalledOperand()]) {
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

Value* getValueByName(std::string name, Function* F) {
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
    }
    return F->getParent()->getNamedValue(name.substr(1));
}

void createAliasGroup(bool shouldAlias, Value* V1, Value* V2) {
    aliasInfo[aliasInfo.empty() ? 0 : aliasInfo.rbegin()->first + 1] = {{V1, V2}, shouldAlias};
    FunctionType* AnnotAliasT = FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Ptr_Type, Basic_Types.Bool_Type, Basic_Types.Int_Type}, false);
    FunctionCallee AnnotAliasCallee = curM->getOrInsertFunction("CoVer_AnnotAlias", AnnotAliasT);
    for (Value* V : {V1, V2}) {
        if (Instruction* I = dyn_cast<Instruction>(V)) {
            CallInst* CI = CallInst::Create(AnnotAliasCallee, {I, Basic_Types.getBool(shouldAlias), Basic_Types.getInt(aliasInfo.rbegin()->first)});
            CI->insertAfter(I);
        }
    }
}

void addToAliasGroup(int idx, Value* V) {
    if (aliasInfo[idx].members.contains(V)) return; // Need to do early return to not double-instrument
    aliasInfo[idx].members.insert(V);
    FunctionType* AnnotAliasT = FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Ptr_Type, Basic_Types.Bool_Type, Basic_Types.Int_Type}, false);
    FunctionCallee AnnotAliasCallee = curM->getOrInsertFunction("CoVer_AnnotAlias", AnnotAliasT);
    if (Instruction* I = dyn_cast<Instruction>(V)) {
        CallInst* CI = CallInst::Create(AnnotAliasCallee, {I, Basic_Types.getBool(aliasInfo[idx].areAliasing), Basic_Types.getInt(idx)});
        CI->insertAfter(I);
    }
}
void removeFromAliasGroup(int group, int idx) {
    errs() << group << " " << idx << "\n";
    Value* V = *std::next(aliasInfo[idx].members.begin(), idx);
    aliasInfo[idx].members.erase(V);
    if (Instruction* I = dyn_cast<Instruction>(V)) {
        for (Instruction* cur = I->getNextNode(); cur && isa<CallBase>(cur); cur = cur->getNextNode()) {
            CallBase* CB = dyn_cast<CallBase>(cur);
            if (CB->getCalledOperand()->getName().starts_with("CoVer_AnnotAlias")) {
                if (dyn_cast<ConstantInt>(CB->getArgOperand(2))->getZExtValue() == idx) {
                    CB->eraseFromParent();
                    break;
                }
            }
        }
    }
    if (aliasInfo[idx].members.empty()) aliasInfo.erase(idx);
}

void setFPTarget(CallBase* indirect, std::set<Function*> targets) {
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
