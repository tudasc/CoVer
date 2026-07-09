#pragma once

#include "llvm/IR/PassManager.h"
#include <llvm/Support/WithColor.h>
#include <llvm/Support/raw_ostream.h>
#include <json/json.h>

namespace llvm {

class AnalysisCleanupPass : public PassInfoMixin<AnalysisCleanupPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
