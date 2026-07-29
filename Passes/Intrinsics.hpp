#include "BasicTypes.hpp"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

namespace llvm {

class IntrinsicsPass : public PassInfoMixin<IntrinsicsPass> {
    public:
        PreservedAnalyses run(Module& M, ModuleAnalysisManager &AM);
    private:
        BasicTypesAnalysis::BasicTypes Basic_Types;

        FunctionCallee calleeHelper(Module& M, std::string name, FunctionType* type);
        void createCallees(Module& M);
        FunctionCallee allocStackCallee;
        FunctionCallee freeStackCallee;
        FunctionCallee globalRegCallee;
        FunctionCallee fallocPointerCallee;
        FunctionCallee fdeallocPointerCallee;

        void instrumentIntrinsics(Module& M);
};

}
