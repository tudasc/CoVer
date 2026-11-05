module;

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/IR/ModuleSlotTracker.h>
#include <llvm/Support/WithColor.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/ReplaceConstant.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/User.h>
#include <llvm/IR/Use.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Transforms/Utils/EscapeEnumerator.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Plugins/PassPlugin.h>

export module LLVMModule;
import std;

export namespace llvm {
    using llvm::Value;
    using llvm::Module;
    using llvm::Function;
    using llvm::Instruction;
    using llvm::Argument;
    using llvm::BasicBlock;
    using llvm::CallBase;
    using llvm::StringRef;
    using llvm::raw_string_ostream;
    using llvm::raw_fd_stream;
    using llvm::isa;
    using llvm::dyn_cast;
    using llvm::demangle;
    using llvm::errs;
    using llvm::DIBuilder;
    using llvm::ConstantPointerNull;
    using llvm::Type;
    using llvm::AnalysisKey;
    using llvm::PassInfoMixin;
    using llvm::AnalysisInfoMixin;
    using llvm::PreservedAnalyses;
    using llvm::ModuleAnalysisManager;
    using llvm::Constant;
    using llvm::ConstantInt;
    using llvm::Metadata;
    using llvm::PointerType;
    using llvm::IntegerType;
    using llvm::FunctionType;
    using llvm::FunctionCallee;
    using llvm::LoadInst;
    using llvm::StoreInst;
    using llvm::AllocaInst;
    using llvm::CallInst;
    using llvm::GetElementPtrInst;
    using llvm::BinaryOperator;
    using llvm::ConstantExpr;
    using llvm::instructions;
    using llvm::getPointerOperand;
    using llvm::convertUsersOfConstantsToInstructions;
    using llvm::IRBuilder;
    using llvm::EscapeEnumerator;
    using llvm::GlobalVariable;
    using llvm::GlobalValue;
    using llvm::User;
    using llvm::Attribute;
    using llvm::AttributeList;
    using llvm::SmallVector;
    using llvm::SmallString;
    using llvm::DISubroutineType;
    using llvm::DISubprogram;
    using llvm::ReplaceInstWithInst;
    using llvm::changeToCall;
    using llvm::GEPOperator;
    using llvm::BranchInst;
    using llvm::SwitchInst;
    using llvm::UnreachableInst;
    using llvm::ReturnInst;
    using llvm::operator==;
    using llvm::operator!=;
    using llvm::getLoadStorePointerOperand;
    using llvm::ReversePostOrderTraversal;
    using llvm::ModuleSlotTracker;
    using llvm::WithColor;
    using llvm::HighlightColor;
    using llvm::DenseMap;
    using llvm::sort;
    using llvm::IntToPtrInst;
    using llvm::ExtractValueInst;
    using llvm::InsertValueInst;
    using llvm::StructType;
    using llvm::ConstantDataArray;
    using llvm::ConstantStruct;
    using llvm::ConstantArray;
    using llvm::Use;
    using llvm::raw_ostream;
    using llvm::raw_fd_ostream;
    using llvm::SExtInst;
    using llvm::ArrayType;
    using llvm::ValueToValueMapTy;
    using llvm::CloneFunctionChangeType;
    using llvm::CloneFunctionInto;
    using llvm::UndefValue;
    using llvm::CastInst;
    using llvm::InvokeInst;
    using llvm::DIType;
    using llvm::DIGlobalVariableExpression;
    using llvm::PassBuilder;
    using llvm::ModulePassManager;
    using llvm::ArrayRef;
    using llvm::FunctionAnalysisManagerModuleProxy;
    using llvm::make_early_inc_range;
    using llvm::GlobalAlias;
    using llvm::MDNode;
    using llvm::NamedMDNode;
    using llvm::DICompileUnit;
    using llvm::operator<;
    using llvm::DICompositeType;
    using llvm::DbgVariableRecord;
    using llvm::DbgRecord;
    using llvm::operator+;
    using llvm::operator<<;
    using llvm::cast;
    using llvm::DebugLoc;
    using llvm::dyn_cast_or_null;
    using llvm::FunctionAnalysisManager;
    using llvm::MDTuple;
    using llvm::PHINode;
    using llvm::PassPluginLibraryInfo;
    using llvm::MemorySSA;
    using llvm::MemorySSAAnalysis;
    using llvm::MemoryAccess;
    using llvm::MemoryEffects;
    using llvm::ModRefInfo;
    using llvm::DominatorTree;
    using llvm::DominatorTreeAnalysis;
}

export namespace llvm::cl {
    using llvm::cl::opt;
    using llvm::cl::init;
    using llvm::cl::desc;
    using llvm::cl::Hidden;
    using llvm::cl::values;
    using llvm::cl::OptionEnumValue; // What the clEnumValN macro creates: OptionEnumValue{name, int(value), description}
}

export namespace llvm::dwarf {
    using llvm::dwarf::DW_TAG_pointer_type;
    using llvm::dwarf::DW_TAG_array_type;
    using llvm::dwarf::DW_ATE_signed;
    using llvm::dwarf::DW_ATE_boolean;
}

export namespace llvm::Intrinsic {
    using llvm::Intrinsic::getOrInsertDeclaration;
    using llvm::Intrinsic::frameaddress;
    using llvm::Intrinsic::stacksave;
}

// Values of LLVM macros, which cannot be exported themselves
export namespace LLVMModule {
    constexpr std::uint32_t PLUGIN_API_VERSION = LLVM_PLUGIN_API_VERSION;
    constexpr char const* VERSION_STRING = LLVM_VERSION_STRING;
}
