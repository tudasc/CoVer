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

enum struct ExecKind { NORMAL, VERBOSE, DRY };
ExecKind cur_execkind = ExecKind::NORMAL;
enum struct LinkKind { LINK, ONLY_COMPILE, ONLY_PREPROCESS };
LinkKind cur_linkkind = LinkKind::LINK;

std::string dest_arg;
std::string opt_level;

std::regex link_file_ending(".*(\\.a|\\.so)");

std::string wrap_target = "@COMPILER_WRAP_TARGET@";

std::vector<std::string> predefined_contracts = {"@PREMADE_CONTRACT_PATHS_EMBED@"};

std::regex llvm_version_regex("version ([0-9]+)\\.[0-9]+\\.[0-9]+");

std::regex source_file_ending("@COMPILE_SRC_FILE_ENDINGS@");
std::string source_file_paths;
std::regex obj_file_ending(".*(\\.o)");
std::string llvmlink_obj_files = "";
std::vector<std::string> source_file_names;

std::string opt_flags = "";

enum struct Instrumentation { NONE, FULL, SAFE };
Instrumentation instr_level;

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
    switch (cur_execkind) {
        case ExecKind::VERBOSE:
        case ExecKind::DRY:
            std::cout << "Wrapper " << (cur_execkind == ExecKind::VERBOSE ? "is executing: " : "would execute: ") << cmd << "\n";
            if (cur_execkind == ExecKind::DRY) break;
        case ExecKind::NORMAL:
            std::cout << exec(cmd);
            break;
    }
}

void printHelp() {
    printf("@EXECUTABLE_WRAPPER_NAME@ is a compiler wrapper to utilize CoVer");
    printf("\nRequires LLVM >= @LLVM_VERSION_MIN@.");
    printf("\n\nUsage: @EXECUTABLE_WRAPPER_NAME@ [--dry-run] [--verbose] [--wrap-target <arg>] [--predefined-contracts] [--allow-multireports] [--instrument-contracts] <compiler-params>");
    printf("\n\t--help: Print this help text and exit");
    printf("\n\t--dry-run: Only show the commands that would be run, but do not perform any");
    printf("\n\t--instrument-contracts: Perform instrumentation for runtime analysis");
    printf("\n\t--verbose: Print commands to be executed");
    printf("\n\t--wrap-target: Set the compiler to wrap around");
    printf("\n\t--predefined-contracts: Automatically include the predefined contract definitions using the -include flag");
    printf("\n\t--allow-multireports: Allow multiple reports of same violated contract");
    printf("\n\n");
}

std::pair<std::string,std::string> parseParams(std::vector<std::string> const& all_args) {
    std::string rem_args_link; // First result, only for linking
    std::string rem_args_compile; // Second result, only for compiling

    for (int i = 0; i < all_args.size(); i++) {
        std::string arg = all_args[i];
        if (arg == "--help") {
            printHelp();
            exit(0);
        } else if (arg == "--dry-run") {
            cur_execkind = ExecKind::DRY;
        } else if (arg == "--verbose") {
            cur_execkind = ExecKind::VERBOSE;
        } else if (arg == "--wrap-target") {
            wrap_target = all_args[++i];
        } else if (arg.starts_with("--instrument-contracts")) {
            instr_level = Instrumentation::SAFE;
            if (arg.starts_with("--instrument-contracts=")) {
                std::string kind = arg.substr(23, std::string::npos); // Cutoff arg and equal sign
                if (kind == "safe") instr_level = Instrumentation::SAFE;
                if (kind == "full") instr_level = Instrumentation::FULL;
                if (kind == "none") instr_level = Instrumentation::NONE;
            }
        } else if (arg == "--allow-multireports") {
            opt_flags += " -cover-allow-multireports=1";
        } else if (arg.starts_with("--generate-json-report")) {
            std::string output_file = "contract_messages.json";
            if (arg.starts_with("--generate-json-report="))
                output_file = arg.substr(23, std::string::npos); // Cutoff arg and equal sign
            opt_flags += " -cover-generate-json-report=" + output_file;
        } else if (arg == "--predefined-contracts") {
            for (std::string path : predefined_contracts) {
                rem_args_compile += " -include " + path;
                rem_args_link += " -include " + path;
            }
        } else if (std::regex_match(arg, source_file_ending)) {
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
            llvmlink_obj_files += " " + arg;
        } else if (arg == "-MT") {
            rem_args_compile += " " + arg + " " + all_args[++i];
        } else {
            rem_args_link += " " + arg;
            rem_args_compile += " " + arg;
            if (arg.starts_with("-O")) {
                opt_level = " " + arg;
            }
        }
    }

    return {rem_args_link, rem_args_compile};
}

void sanityCheckCompiler() {
    std::string cmd;

    // Check for LLVM-based compiler
    cmd = wrap_target + " --version | head -n 1";
    std::string res  = exec(cmd);
    if (res.find("clang") == std::string::npos && res.find("flang") == std::string::npos) {
        std::cerr << "Unknown compiler \"" << res.substr(0, res.size()-1) << "\"!\n";
        std::cerr << "Make sure to use an LLVM-based compiler that supports outputting bitcode.\n";
        std::cerr << "The wrapper will now exit\n";
        exit(-1);
    }

    // Check for LLVM version
    std::smatch matches;
    std::regex_search(res, matches, llvm_version_regex);
    if (matches.size() < 2) {
        std::cerr << "Unknown LLVM Version! This may cause issues!\n";
    } else if (std::stoi(matches[1]) < std::stoi("@LLVM_VERSION_MIN@")) { // Ugly, but avoids linter error and is compiled away in O3 anyway
        std::cerr << "Unsupported LLVM Version " << matches[1] << "! Expect issues!\n";
    }
}

int main(int argc, const char** argv) {
    std::vector<std::string> all_args;

    if (argc <= 1) {
        printHelp();
        exit(0);
    }

    all_args.assign(argv + 1, argv + argc);
    std::pair<std::string,std::string> rem_args = parseParams(all_args);

    sanityCheckCompiler();

    // Generate IR for source files
    std::string bitcode_files;
    if (!source_file_paths.empty()) {
        std::string common_options = " -fPIC -g -emit-llvm -Xclang -disable-O0-optnone ";
        execSafe(wrap_target + common_options + (cur_linkkind < LinkKind::ONLY_PREPROCESS ? "-c" : "-E") + " -I\"@CONTR_INCLUDE_PATH@\"" + rem_args.second + source_file_paths + (cur_linkkind > LinkKind::LINK ? dest_arg : ""));

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
                if (cur_execkind == ExecKind::DRY) [[unlikely]] {
                    std::cout << "Would move: " << source << " to " << destination << "\n";
                } else {
                    if (cur_execkind == ExecKind::VERBOSE) std::cout << "Moving: " << source << " to " << destination << "\n";
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

    // Perform link and analysis steps
    std::string tmpfile = std::filesystem::temp_directory_path().string() + "/contrPlugin_XXXXXX";
    int fd = mkstemp(tmpfile.data());
    execSafe("llvm-link" + bitcode_files + llvmlink_obj_files + " -o " + tmpfile);

    // Call LLVM passes
    std::string passlist = "contractVerifierPreCall,contractVerifierPostCall,contractVerifierRelease,contractPostProcess";
    if (instr_level > Instrumentation::NONE) passlist += ",instrumentContracts";
    if (instr_level == Instrumentation::SAFE) opt_flags += " -cover-instrument-type=safe";
    if (instr_level == Instrumentation::FULL) opt_flags += " -cover-instrument-type=full";
    execSafe("opt -load-pass-plugin \"@CONTR_PLUGIN_PATH@\" -passes='" + passlist + "' " + opt_flags + " " + tmpfile + " -o " + tmpfile + ".opt");
    close(fd);

    // Finalize executable
    execSafe("llc -filetype=obj --relocation-model=pic" + opt_level + " " + tmpfile + ".opt -o " + tmpfile + ".opt.o");
    execSafe(wrap_target + " -fPIC -lm -ldl -lpthread -g -I\"@CONTR_INCLUDE_PATH@\"" + rem_args.first + " " + tmpfile + ".opt.o" + dest_arg);
    return 0;
}
