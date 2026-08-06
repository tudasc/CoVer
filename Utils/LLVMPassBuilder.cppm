module;

#include <llvm/Passes/PassBuilder.h>
#include <llvm/Plugins/PassPlugin.h>

export module LLVMPassBuilder;

export namespace llvm {
    using llvm::PassBuilder;
    using llvm::PassPluginLibraryInfo;
}

// Macros do not cross module boundaries, so re-expose the plugin ABI values
// that llvmGetPassPluginInfo() has to report.
export inline constexpr uint32_t CoVerPluginAPIVersion = LLVM_PLUGIN_API_VERSION;
export inline constexpr const char* CoVerLLVMVersionString = LLVM_VERSION_STRING;
