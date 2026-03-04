#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <string>
#include <filesystem>
#include <array>
#include <unistd.h>
#include <utility>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>

#define SET_FROM_ENV(x,y) (std::getenv(y) ? std::getenv(y) : x)

using namespace llvm;

// 1. Create a category to group your wrapper's specific options cleanly in the help output
static cl::OptionCategory WrapperCategory("CoVer compile wrapper options");

// 2. Define the flags
static cl::opt<bool> ExecDryRun("dry-run",
    cl::desc("Only show the commands that would be run, but do not perform any. Implies --verbose"),
    cl::cat(WrapperCategory));

static cl::opt<bool> ExecVerbose("verbose",
    cl::desc("Print commands to be executed"),
    cl::cat(WrapperCategory));

static cl::opt<std::string> WrapTarget("wrap-target",
    cl::desc("Set the compiler to wrap around"),
    cl::value_desc("compiler path"),
    cl::init(SET_FROM_ENV("@COMPILER_WRAP_TARGET@","COVER_WRAP_TARGET_@COMPILER_WRAP_TARGET@")),
    cl::cat(WrapperCategory));

static cl::opt<bool> PredefinedContracts("predefined-contracts",
    cl::desc("Automatically include the predefined contract definitions using the -include flag"),
    cl::cat(WrapperCategory));

static cl::opt<bool> AllowMultiReports("allow-multireports",
    cl::desc("Allow multiple reports of same violated contract"),
    cl::cat(WrapperCategory));

static cl::opt<std::string> GenerateJSONReport("generate-json-report",
    cl::desc("Generate JSON report or detected errors. Path defaults to contract_messages.json"),
    cl::ValueOptional,
    cl::value_desc("JSON output path"),
    cl::cat(WrapperCategory));

// String option with ValueOptional to handle "full", "funconly", and "filtered=path.json"
static cl::opt<std::string> InstrumentContracts("instrument-contracts",
    cl::desc("Perform instrumentation for runtime analysis.\n"
             "  full: Full instrumentation (default if instrumenting)\n"
             "  funconly: Disable costly memory instrumentation\n"
             "  filtered[=<detection json>]: Only instrument potential issues from static analysis."),
    cl::ValueOptional,
    cl::value_desc("(full|funconly|filtered)"),
    cl::cat(WrapperCategory));

static cl::list<std::string> CompilerParams(cl::Sink,
    cl::desc("<compiler params>"));

static cl::opt<std::string> Autocomplete("autocomplete",
    cl::desc("Provide autocompletion for shell"),
    cl::Hidden);

enum struct LinkKind { LINK, ONLY_COMPILE, ONLY_PREPROCESS };
LinkKind cur_linkkind = LinkKind::LINK;

std::string dest_arg;
std::string opt_level;

std::regex const link_file_ending(".*(\\.a|\\.so)");

std::string compiler_ident;

auto constexpr predefined_contract_includes = std::to_array({"@PREMADE_CONTRACT_INCLUDES@"});
auto constexpr predefined_contract_sources = std::to_array({"@PREMADE_CONTRACT_SOURCES@"});

std::regex const llvm_version_regex("version ([0-9]+)\\.[0-9]+\\.[0-9]+");

std::regex const source_file_ending("@COMPILE_SRC_FILE_ENDINGS@");
std::string source_file_paths;
std::regex const obj_file_ending(".*(\\.o)");
std::string bitcode_files;
std::vector<std::string> source_file_names;
std::vector<std::string> link_time_sources; // For predef fort contracts

std::string opt_flags = "";

std::string exec(std::string const& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
    int ret = WEXITSTATUS(pclose(pipe));
    if (ret != 0) {
        exit(ret);
    }
    return result;
}

void execSafe(std::string const& cmd) {
    if (ExecVerbose || ExecDryRun)
        std::cout << "Wrapper " << (ExecDryRun ? "would execute: " : "is executing: ") << cmd << "\n";
    if (!ExecDryRun) std::cout << exec(cmd);
}

std::string getOptParam(std::string param, std::string full) {
    if (full.starts_with(param + "=")) {
        return full.substr((param + "=").size(), std::string::npos);
    }
    return "";
}

std::pair<std::string,std::string> parseWrapperParams(std::pair<std::string,std::string>& rem_args) {
    std::string& rem_args_link = rem_args.first; // First result, only for linking
    std::string& rem_args_compile = rem_args.second; // Second result, only for compiling

    if (AllowMultiReports) opt_flags += " -cover-allow-multireports=1";

    if (InstrumentContracts.getNumOccurrences() && InstrumentContracts.empty()) InstrumentContracts = "full";
    if (!InstrumentContracts.empty()) opt_flags += " -cover-instrument-type=\"" + InstrumentContracts + "\"";

    if (GenerateJSONReport.getNumOccurrences() && GenerateJSONReport.empty()) GenerateJSONReport = "contract_messages.json";
    if (!GenerateJSONReport.empty()) opt_flags += " -cover-generate-json-report=" + GenerateJSONReport;

    if (PredefinedContracts) {
        for (std::string path : predefined_contract_includes) {
            rem_args_compile += !path.empty() ? " -include " + path : "";
        }
        link_time_sources.insert(link_time_sources.end(), predefined_contract_sources.begin(), predefined_contract_sources.end());
    }

    return {rem_args_link, rem_args_compile};
}

std::pair<std::string,std::string> parseCompilerParams(std::vector<std::string> const& all_args, std::pair<std::string,std::string>& rem_args) {
    std::string& rem_args_link = rem_args.first; // First result, only for linking
    std::string& rem_args_compile = rem_args.second; // Second result, only for compiling

    for (int i = 0; i < all_args.size(); i++) {
        std::string arg = all_args[i];
        if (std::regex_match(arg, source_file_ending)) {
            source_file_paths += " " + arg;
            source_file_names.push_back(std::filesystem::path(arg).stem());
        } else if (arg == "-o") {
            dest_arg = " " + arg + " " + all_args[++i];
        } else if (arg == "-c") {
            cur_linkkind = LinkKind::ONLY_COMPILE;
        } else if (arg == "-E") {
            cur_linkkind = LinkKind::ONLY_PREPROCESS;
        } else if (std::regex_match(arg, link_file_ending)) {
            rem_args_link += " " + arg;
        } else if (std::regex_match(arg, obj_file_ending)) {
            bitcode_files += " " + arg;
        } else if (arg == "-MT") {
            rem_args_compile += " " + arg + " " + all_args[++i];
        } else if (arg.starts_with("-O")) {
            opt_level = arg;
        } else {
            rem_args_link += " " + arg;
            rem_args_compile += " " + arg;
        }
    }

    return {rem_args_link, rem_args_compile};
}

void sanityCheckCompiler() {
    std::string cmd;

    // Check for LLVM-based compiler
    cmd = WrapTarget + " --version | head -n 1";
    compiler_ident  = exec(cmd);
    if (compiler_ident.find("clang") == std::string::npos && compiler_ident.find("flang") == std::string::npos) {
        std::cerr << "Unknown compiler \"" << compiler_ident.substr(0, compiler_ident.size()-1) << "\"!\n";
        std::cerr << "Make sure to use an LLVM-based compiler that supports outputting bitcode.\n";
        std::cerr << "The wrapper will now exit\n";
        exit(-1);
    }

    // Check for LLVM version
    std::smatch matches;
    std::regex_search(compiler_ident, matches, llvm_version_regex);
    if (matches.size() < 2) {
        std::cerr << "Unknown LLVM Version! This may cause issues!\n";
    } else if (std::stoi(matches[1]) < std::stoi("@LLVM_VERSION_MIN@")) { // Ugly, but avoids linter error and is compiled away in O3 anyway
        std::cerr << "Unsupported LLVM Version " << matches[1] << "! Expect issues!\n";
    }
}

int main(int argc, const char** argv) {
    cl::HideUnrelatedOptions(WrapperCategory);
    cl::ParseCommandLineOptions(argc, argv,
        "@EXECUTABLE_WRAPPER_NAME@ - CoVer Compiler Wrapper\n");

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
            StringRef OptName = It.getKey();
            if (It.getValue()->getOptionHiddenFlag() != cl::NotHidden) continue;
            if (OptName.starts_with(Prefix)) {
                outs() << "--" << OptName << "\n";
            }
        }
        return 0;
    }

    std::pair<std::string,std::string> rem_args;
    parseWrapperParams(rem_args);
    parseCompilerParams(CompilerParams, rem_args);

    sanityCheckCompiler();

    // Generate IR for source files
    if (!source_file_paths.empty()) {
        std::string common_options = " -fPIC -g -emit-llvm " + std::string(compiler_ident.find("clang") != std::string::npos ? "-Xclang -disable-O0-optnone " : "");
        execSafe(WrapTarget + common_options + (cur_linkkind < LinkKind::ONLY_PREPROCESS ? "-c" : "-E") + " -I\"@CONTR_INCLUDE_PATH@\"" + rem_args.second + source_file_paths + (cur_linkkind > LinkKind::LINK ? dest_arg : ""));

        if (cur_linkkind == LinkKind::ONLY_COMPILE && dest_arg.empty()) {
            // No output dir, but want "object" files. Rename generated .ll to .o
            for (std::string file : source_file_names) {
                std::filesystem::rename(file + ".bc", file + ".o");
            }
        } else if (cur_linkkind == LinkKind::LINK) {
            // Linking, so move bitcode files to tmp dir
            for (std::string file : source_file_names) {
                std::string source = file + ".bc";
                std::string destination = std::filesystem::temp_directory_path().string() + "/contrPlugin_XXXXXX";
                int fd = mkstemp(destination.data());
                if (ExecDryRun) [[unlikely]] {
                    std::cout << "Would move: " << source << " to " << destination << "\n";
                } else {
                    if (ExecVerbose) std::cout << "Moving: " << source << " to " << destination << "\n";
                    // Cannot use rename, because it is on other fs. Copy-delete instead
                    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
                    std::filesystem::remove(source);
                    bitcode_files += " " + destination;
                }
                close(fd);
            }
        }
    }

    // If not linking, return early
    if (cur_linkkind != LinkKind::LINK) return 0;

    // Add source contract files if needed
    for (std::string file : link_time_sources) {
        if (file.empty()) continue;
        std::string destination = std::filesystem::temp_directory_path().string() + "/contrPlugin_predef_XXXXXX";
        int fd = mkstemp(destination.data());
        execSafe(WrapTarget + " -g -c -emit-llvm -I\"@CONTR_INCLUDE_PATH@\" " + file + " -o " + destination);
        bitcode_files += " " + destination;
    }

    // Perform link and analysis steps
    std::string tmpfile = std::filesystem::temp_directory_path().string() + "/contrPlugin_XXXXXX";
    int fd = mkstemp(tmpfile.data());
    execSafe("llvm-link" + bitcode_files + " -o " + tmpfile);

    // Call LLVM passes
    std::string passlist = "function(sroa),contractVerifierPreCall,contractVerifierPostCall,contractVerifierRelease,contractPostProcess";
    if (!opt_level.empty()) {
        passlist += ",default<" + opt_level.substr(1) + ">"; // opt_level substr cuts "-" from "-O<num>"
    }
    if (!InstrumentContracts.empty()) {
        // Need instrumentation, so add instr pass...
        passlist += ",instrumentContracts";
        // ...and link against analyser. Need to hackily link against stdlib as well for C code
        rem_args.first += " -Wl,--whole-archive @COVER_DYNAMIC_ANALYSER_PATH@ -Wl,-no-whole-archive -lstdc++";
    }
    execSafe("opt --load-pass-plugin=\"@DSA_PLUGIN_PATH@\" --load-pass-plugin \"@CONTR_PLUGIN_PATH@\" -passes='" + passlist + "' " + opt_flags + " " + tmpfile + " -o " + tmpfile + ".opt");
    close(fd);

    // Finalize executable
    execSafe("llc -filetype=obj --relocation-model=pic " + opt_level + " " + tmpfile + ".opt -o " + tmpfile + ".opt.o");
    execSafe(WrapTarget + " -fPIC -lm -ldl -lpthread -g -I\"@CONTR_INCLUDE_PATH@\"" + rem_args.first + " " + tmpfile + ".opt.o" + dest_arg);
    return 0;
}
