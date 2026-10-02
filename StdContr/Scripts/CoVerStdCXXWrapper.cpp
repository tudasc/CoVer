#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <llvm/Support/CommandLine.h>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

using namespace llvm;

// Create a category to group wrapper specific options in the help output
static cl::OptionCategory WrapperCategory("CoVerStdCXX compile wrapper options");

static cl::opt<bool> ExecDryRun("dry-run",
    cl::desc("Only show the commands that would be run, but do not perform any. Implies --verbose"),
    cl::cat(WrapperCategory));

static cl::opt<bool> ExecVerbose("verbose",
    cl::desc("Print commands to be executed"),
    cl::cat(WrapperCategory));

static cl::opt<std::string> ClangPath("wrap-target-clang",
    cl::desc("Set the clang++ compiler (for the frontend plugin)"),
    cl::value_desc("clang compiler path"),
    cl::init("clang++"),
    cl::cat(WrapperCategory));

static cl::opt<std::string> GCCPath("wrap-target-gcc",
    cl::desc("Set the g++ compiler (for the compilation + backend plugin)"),
    cl::value_desc("g++ compiler path"),
    cl::init("g++"),
    cl::cat(WrapperCategory));

static cl::opt<std::string> ContractFile("contract-file",
    cl::desc("Set to path to the header with CoVer contracts to convert"),
    cl::init(""),
    cl::cat(WrapperCategory));

static cl::opt<std::string> TempPath("temp-path",
    cl::desc("Set where to store temporary files (source with converted contracts/wrappers, backend wrapfile)"),
    cl::init("/tmp/CoVerStdCXX"),
    cl::cat(WrapperCategory));

static cl::list<std::string> CompilerParams(cl::Sink,
    cl::desc("<compiler params>"));

static cl::opt<std::string> Autocomplete("autocomplete",
    cl::desc("Provide autocompletion for shell"),
    cl::Hidden);

std::string_view constexpr common_flags = " -g -std=c++26 -fcontracts -I\"@CONTR_INCLUDE_PATH@\" ";

std::regex const source_file_ending(".*(\\.cpp|\\.cc|\\.cxx|\\.c)$");
std::regex const link_file_ending(".*(\\.a|\\.so)");
std::regex const obj_file_ending(".*(\\.o|\\.ll)");

std::vector<std::string> source_files;
std::string bitcode_files;
std::string dest_arg;
std::string opt_level;
std::string stop_flag;
bool isLinking = true;

// Files kept in the temp path between invocations of one build
std::string markerFile() { return TempPath + "/frontend_done"; }       // Frontend has run, wrapfile exists
std::string usedListFile() { return TempPath + "/cover_used_funcs"; }  // Functions the program references
std::string flagsFile() { return TempPath + "/cover_compile_flags"; }  // Compile flags to build the wrappers with

void removeTemporaries() {
    std::filesystem::remove(markerFile());
    std::filesystem::remove(usedListFile());
    std::filesystem::remove(flagsFile());
}

std::string exec(std::string const& cmd, bool interactive = true) {
    int rc;
    std::string res;
    if (interactive) {
        rc = std::system(cmd.c_str());
        res = "";
    } else {
        std::array<char, 128> buffer;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            throw std::runtime_error("popen() failed!");
        }
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            res += buffer.data();
        }
        rc = WEXITSTATUS(pclose(pipe));
    }
    if (rc != 0) {
        // Cleanup before aborting
        removeTemporaries();
        exit(rc);
    }
    return res;
}

void execSafe(std::string const& cmd) {
    if (ExecVerbose || ExecDryRun)
        std::cout << "Wrapper " << (ExecDryRun ? "would execute: " : "is executing: ") << cmd << "\n";
    if (!ExecDryRun) exec(cmd);
}

std::string getOptParam(std::string param, std::string full) {
    if (full.starts_with(param + "=")) {
        return full.substr((param + "=").size(), std::string::npos);
    }
    return "";
}

std::pair<std::string,std::string> parseCompilerParams(std::vector<std::string> const& all_args) {
    std::string rem_args_link;
    std::string rem_args_compile;

    for (int i = 0; i < all_args.size(); i++) {
        std::string arg = all_args[i];
        if (std::regex_match(arg, source_file_ending)) {
            source_files.push_back(arg);
        } else if (arg == "-o") {
            dest_arg = " " + arg + " " + all_args[++i];
        } else if (arg == "-c" || arg == "-E") {
            isLinking = false;
            stop_flag = " " + arg;
        } else if (std::regex_match(arg, link_file_ending)) {
            rem_args_link += " " + arg;
        } else if (std::regex_match(arg, obj_file_ending)) {
            bitcode_files += " " + arg;
        } else if (arg == "-MT") {
            rem_args_compile += " " + arg + " " + all_args[++i];
        } else if (arg.starts_with("-O")) {
            opt_level = " " + arg;
        } else {
            rem_args_link += " " + arg;
            rem_args_compile += " " + arg;
        }
    }

    return {rem_args_link, rem_args_compile};
}

int main(int argc, const char** argv) {
    cl::HideUnrelatedOptions(WrapperCategory);
    cl::ParseCommandLineOptions(argc, argv,
        "CoVerStdCXX - Compiler Wrapper which uses C++26 contracts to verify CoVer contracts\n");

    if (argc <= 1) {
        cl::PrintHelpMessage();
        exit(0);
    }

    // Provide --autocomplete command for bash-completion
    if (Autocomplete.getNumOccurrences() > 0) {
        StringRef Input = Autocomplete;

        // Split by comma
        SmallVector<StringRef, 4> args;
        Input.split(args, ',');
        if (args.empty()) return 0;
        StringRef Prefix = args.back();

        if (!Prefix.starts_with('-')) return 0; // Default to File input

        // Strip the leading dashes from the bash input
        Prefix = Prefix.ltrim('-');
        for (auto &It : cl::getRegisteredOptions()) {
            StringRef OptName = It.getFirst();
            if (It.getSecond()->getOptionHiddenFlag() != cl::NotHidden) continue;
            if (OptName.starts_with(Prefix)) {
                outs() << "--" << OptName << "\n";
            }
        }
        return 0;
    }

    if (ContractFile.empty()) {
        errs() << "No contract header file specified! Aborting\n";
        return 1;
    }

    std::string rem_link, rem_compile;
    std::tie(rem_link, rem_compile) = parseCompilerParams(CompilerParams);

    std::string FrontendCmd = ClangPath + " -I\"@CONTR_INCLUDE_PATH@\" "
                            + "-fplugin=@COVER_STDCXX_FRONTEND_PATH@ "
                            + "-fplugin-arg-CoVerStdCXXFrontend-output-folder=\"" + TempPath + "\" "
                            + "-g -x c++-header " + ContractFile;

    if (!std::filesystem::exists(markerFile())) {
        // Run frontend pass on input header, generating the wrapfile for the backend
        execSafe(FrontendCmd);

        // List of referenced functions, filled by the backend for each unit. Not truncated, as parallel
        // first invocations may race here. A stale list only adds wrappers, it is removed after linking
        std::ofstream{usedListFile(), std::ios::app};
        std::ofstream(flagsFile()) << rem_compile << opt_level;
        std::ofstream{markerFile()};
    }

    std::string GCCPluginLoad = " -g -fplugin=@COVER_STDCXX_BACKEND_PATH@ -fplugin-arg-CoVerStdCXXBackend-list=" + TempPath + "/cover_wrapfile "
                              + "-fplugin-arg-CoVerStdCXXBackend-used-list=" + usedListFile() + " ";

    if (!isLinking) {
        std::string sources;
        for (std::string const& src : source_files) sources += " " + src;
        execSafe(GCCPath + GCCPluginLoad + rem_compile + opt_level + common_flags + stop_flag + sources + dest_arg);
        return 0;
    }

    // Compile sources on their own first, so their used functions are known before generating the wrappers
    std::string objects = bitcode_files;
    std::vector<std::string> source_objects;
    for (std::string const& src : source_files) {
        std::string obj = TempPath + "/cover_src_" + std::to_string(source_objects.size()) + ".o";
        execSafe(GCCPath + GCCPluginLoad + rem_compile + opt_level + common_flags + " -c " + src + " -o " + obj);
        source_objects.push_back(obj);
        objects += " " + obj;
    }

    // Generate wrappers only for the functions the program calls, then compile them
    // with the flags of the first invocation, as those can find the contract header's includes
    execSafe(FrontendCmd + " -fplugin-arg-CoVerStdCXXFrontend-used-list=" + usedListFile());
    std::string wrapper_flags = rem_compile + opt_level;
    if (std::ifstream in(flagsFile()); in) std::getline(in, wrapper_flags);
    execSafe(GCCPath + " " + wrapper_flags + " -fplugin=@COVER_STDCXX_BACKEND_PATH@ -fplugin-arg-CoVerStdCXXBackend-runtime -c" + common_flags + TempPath + "/include.cpp -o " + TempPath + "/include.o");

    // Finish normal compilation process.
    // include.o has to stay behind the objects here!
    #warning TODO check if need to remove -lstdc++exp once contract and std::stacktrace support matures
    execSafe(GCCPath + rem_link + opt_level + common_flags + objects + dest_arg + " " + TempPath + "/include.o -Wl,--whole-archive @COVER_INTRINSICS_LIB_PATH@ -Wl,--no-whole-archive -lstdc++ -lstdc++exp");

    // Delete old temporaries at end
    for (std::string const& obj : source_objects) std::filesystem::remove(obj);
    removeTemporaries();
}
