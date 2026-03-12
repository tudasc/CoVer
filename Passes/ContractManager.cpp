#include "ContractManager.hpp"

#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/IPO/Attributor.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/WithColor.h>
#include <memory>

#include <string>
#include <vector>
#include "../LangCode/ContractDataExtractor.hpp"
#include "ContractTree.hpp"

#include "ContractPassUtility.hpp"

using namespace llvm;

static cl::opt<bool> ClMultiReports(
    "cover-allow-multireports", cl::init(false),
    cl::desc("Allow multiple error reports of the same contract"),
    cl::Hidden);

static std::optional<std::string> getFuncName(CallBase* FuncCall) {
    if (!FuncCall->getCalledFunction()) {
        if (!FuncCall->getCalledOperand()->getName().empty()) {
            return FuncCall->getCalledOperand()->getName().data();
        } else {
            return std::nullopt;
        }
    }
    return FuncCall->getCalledFunction()->getName().data();
}

ContractManagerAnalysis::ContractDatabase ContractManagerAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
    curDatabase.start_time = std::chrono::system_clock::now();
    curDatabase.allowMultiReports = ClMultiReports;

    errs() << "Running Contract Manager on Module: " << M.getName() << "\n";

    extractFromAnnotations(M);
    extractFromFunction(M);

    // Annotations done, now add value pairs to database
    for (GlobalVariable& GV : M.globals()) {
        if (GV.getName().starts_with("ContractValueInfo_")) {
            Constant* data = GV.getInitializer();
            StringRef name =  dyn_cast<ConstantDataArray>(dyn_cast<GlobalVariable>(data->getOperand(0))->getInitializer())->getAsCString();
            Value* val = data->getOperand(1);
            if (ConstantExpr* CE = dyn_cast<ConstantExpr>(data->getOperand(1))) {
                if (isa<IntToPtrInst>(CE->getAsInstruction())) {
                    val = CE->getOperand(0);
                }
            }
            addValueDefinition(name.str(), val);
        }
    }

    std::stringstream s;
    s << "CoVer: Parsed contracts after " << std::fixed << std::chrono::duration<double>(std::chrono::system_clock::now() - curDatabase.start_time).count() << "s\n";
    errs() << s.str();

    return curDatabase;
}

void ContractManagerAnalysis::extractFromAnnotations(const Module& M) {
    GlobalVariable* Annotations = M.getGlobalVariable("llvm.global.annotations");
    if (Annotations == nullptr) {
        errs() << "Note: No contract annotations found in Function declarations.\n";
        return;
    }

    Constant* ANNValues = Annotations->getInitializer();

    for (Use& annUse : ANNValues->operands()) {
        ConstantStruct *ANN = dyn_cast<ConstantStruct>(annUse);
        StringRef ANNStr = dyn_cast<ConstantDataArray>(dyn_cast<GlobalVariable>(ANN->getOperand(1))->getInitializer())->getAsCString();
        Function* F =  dyn_cast<Function>(ANN->getOperand(0));
        addContract(ANNStr.str(), F);
    }
}

void ContractManagerAnalysis::extractFromFunction(Module& M) {
    std::vector<Function*> to_remove;
    for (Function& F : M) {
        if (F.getName().starts_with("contract_definitions_fort")) {
            if (F.isDeclaration()) {
                errs() << "Contract definition by function body failed, function body not found!\n";
                continue;
            }
            BasicBlock& BB = F.getEntryBlock(); // Exactly one basic block allowed, so this is ok
            for (Instruction& I : BB) {
                if (CallBase* CB = dyn_cast<CallBase>(&I)) {
                    // Only care about this intrinsic
                    #warning TODO probably should figure out a less hacky way.
                    if (CB->getCalledFunction()->getName() != "llvm.memmove.p0.p0.i64" && CB->getCalledFunction()->getName() != "llvm.memcpy.p0.p0.i64") continue;
                    // Add CONTRACT { ... } brace. Its explicitly needed for C(++) to make sure we are not parsing irrelevant stuff,
                    // but for fortran its already implicit in declare_contract, making it superfluous
                    std::string CallStr = ((ConstantDataArray*)((GlobalVariable*)CB->getArgOperand(1))->getInitializer())->getAsString().str();
                    // Call is from memmove -> insertvalue -> extractvalue -> funccall. on -O0, and memcpy -> funccall on -O1 and above
                    CallBase* ContrCall = (CallBase*)(isa<CallBase>(*CB->getArgOperand(0)->user_begin()) ? *CB->getArgOperand(0)->user_begin() : *CB->getArgOperand(0)->user_begin()->user_begin()->user_begin()->user_begin());
                    if (ContrCall->getCalledOperand()->getName() == "declare_contract_") {
                        const Function* ContrSup = (Function*)ContrCall->getArgOperand(0);
                        if (ContrSup->hasOneUser()) continue; // Only used here where the contract is defined. No need to verify.
                        bool has_callsite = false;
                        for (const User* U : ContrSup->users() ) {
                            if (const CallBase* CB = dyn_cast<CallBase>(U)) {
                                if (CB->getCalledOperand() == ContrSup) {
                                    has_callsite = true;
                                    break;
                                }
                            }
                        }
                        if (!has_callsite) continue;
                        addContract("CONTRACT { " + CallStr + " }", (Function*)(ContrCall->getArgOperand(0)));
                    } else if (ContrCall->getCalledOperand()->getName() == "declare_value_") {
                        #warning really super duper should find out a less hacky way
                        // If contr value is nullptr, its trivial
                        if (ContrCall->getArgOperand(1) == ConstantPointerNull::getNullValue(PointerType::get(M.getContext(), 0))) {
                            addValueDefinition(CallStr, ContrCall->getArgOperand(1));
                            continue;
                        }

                        // This is the start of a dirty heuristic. If it breaks in the future, good luck to you
                        // Start at the third instr after memmove. This skips the string parameter init (putting it in a struct with its length)
                        Instruction* ImportantInst = CB->getNextNode()->getNextNode()->getNextNode();
                        Value* result = nullptr;

                        // Possibility 1: StoreInst saving an integer into a ptr, where the integer is what we want
                        if (StoreInst* SI = dyn_cast<StoreInst>(ImportantInst)) {
                            if (isa<ConstantInt>(SI->getValueOperand())) {
                                result = SI->getValueOperand();
                            }
                        }

                        // Possibility 2: SExtInst, then stuff, then an InsertValueInst with index 0 where that is what we want
                        if (SExtInst* SEI = dyn_cast<SExtInst>(ImportantInst)) {
                            for (Instruction* cur = SEI; cur && !isa<CallInst>(cur); cur = cur->getNextNode()) {
                                if (InsertValueInst* IVI = dyn_cast<InsertValueInst>(cur)) {
                                    if (IVI->hasIndices() && IVI->getIndices()[0] == 0) {
                                        result = IVI->getInsertedValueOperand();
                                    }
                                }
                            }
                        }

                        // Possibility 3: GEPInst, directly after a StoreInst or LoadInst with correct value
                        if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(ImportantInst)) {
                            if (StoreInst* SI = dyn_cast<StoreInst>(GEP->getNextNode())) {
                                result = SI->getValueOperand();
                            } else if (LoadInst* LI = dyn_cast<LoadInst>(GEP->getNextNode())) {
                                result = LI->getPointerOperand();
                            }
                        }

                        // Check for Fortran mangling stuff
                        if (!result) {
                            errs() << "Could not decipher call to Declare_Value for \n" << CallStr << "!\n";
                        } else {
                            if (result->getName().starts_with("_QQ") && isa<GlobalVariable>(result)) {
                                StringRef result_stem = result->getName().split('.').first;
                                for (GlobalVariable& GV : M.globals()) {
                                    if (GV.getName().starts_with(result_stem)) {
                                        addValueDefinition(CallStr, &GV);
                                    }
                                }
                            } else {
                                addValueDefinition(CallStr, result);
                            }
                        }
                    }
                }
            }
            // This function is unreachable, and should definitely not be analysed, so no need to compile. Drop it
            to_remove.push_back(&F);
        }
    }
    // Remove unneeded functions
    for (Function* F : to_remove)
        F->eraseFromParent();
    if (to_remove.empty()) {
        errs() << "Note: No contract declarations found using Declare_Contract calls\n";
    }
}

void ContractManagerAnalysis::addContract(std::string contract, Function* F) {
    std::optional<ContractData> Data = getContractData(contract);
    if (!Data) return;
    if (IS_DEBUG) errs() << "Found contract for function " << F->getName() << " with content: " << contract << "\n";

    // Finally have contract data
    Contract newCtr{F, contract, *Data};

    // Add normal contract
    curDatabase.Contracts.push_back(newCtr);

    // Create and add Linearized Contract
    std::vector<std::shared_ptr<ContractExpression>> PreLin;
    for (const std::shared_ptr<ContractFormula> contrF : newCtr.Data.Pre) {
        std::vector<std::shared_ptr<ContractExpression>> contrFLin = linearizeContractFormula(contrF);
        PreLin.insert( PreLin.end(), contrFLin.begin(), contrFLin.end() );
    }
    std::vector<std::shared_ptr<ContractExpression>> PostLin;
    for (const std::shared_ptr<ContractFormula> contrF : newCtr.Data.Post) {
        std::vector<std::shared_ptr<ContractExpression>> contrFLin = linearizeContractFormula(contrF);
        PostLin.insert( PostLin.end(), contrFLin.begin(), contrFLin.end() );
    }

    LinearizedContract lContract = { F, contract, PreLin, PostLin, newCtr.DebugInfo};
    curDatabase.LinearizedContracts.push_back(lContract);

    // Append tag database
    curDatabase.Tags[newCtr.F].insert(curDatabase.Tags[newCtr.F].end(), newCtr.Data.Tags.begin(), newCtr.Data.Tags.end());
}

void ContractManagerAnalysis::addValueDefinition(std::string name, Value* val) {
    if (IS_DEBUG) {
        WithColor(errs(), HighlightColor::Remark) << "[ContractManager] IR Value \"";
        val->print(WithColor(errs(), HighlightColor::Remark)),
        WithColor(errs(), HighlightColor::Remark) << "\" stored for contract value \"" << name << "\"\n";
    }
    curDatabase.ContractVariableData[name].insert(val);
}

const std::vector<std::shared_ptr<ContractExpression>> ContractManagerAnalysis::linearizeContractFormula(const std::shared_ptr<ContractFormula> contrF) {
    if (contrF->Children.empty()) {
        return { std::static_pointer_cast<ContractExpression>(contrF) };
    }
    std::vector<std::shared_ptr<ContractExpression>> exprs;
    for (std::shared_ptr<ContractFormula> form : contrF->Children ) {
        std::vector<std::shared_ptr<ContractExpression>> linChild = linearizeContractFormula(form);
        exprs.insert( exprs.end(), linChild.begin(), linChild.end() );
    }
    return exprs;
}
