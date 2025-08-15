#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

std::string launcher;
std::string executable;
std::string exec_str;

void printHelp() {
    printf("CoVerDynamicLauncher is a dynamic contract verifier for CoVer contracts");
    printf("\n\nUsage: CoVerDynamicLauncher [--launcher <arg>] -- <CoVer-instrumented executable>");
    printf("\n\t--help: Print this help text and exit");
    printf("\n\t--launcher: Specify a program launcher such as mpiexec, oshrun");
    printf("\n\n");
}

int main(int argc, const char** argv) {
    std::vector<std::string> all_args;
    all_args.assign(argv + 1, argv + argc);

    // Nothing specified, just print usage and exit
    if (all_args.size() <= 1) { printHelp(); return 0; }

    for (int i = 0; i < all_args.size(); i++) {
        std::string arg = all_args[i];
        if (arg == "--help") {
            printHelp();
            return 0;
        } else if (arg == "--launcher") {
            launcher = all_args[++i];
        } else if (arg == "--") {
            executable = all_args[++i];
            for (; i < all_args.size(); i++) {
                exec_str += all_args[i] + " ";
            }
        }
    }

    setenv("LD_PRELOAD", "@COVER_DYNAMIC_ANALYSER_PATH@", 1);
    setenv("COVER_DYNAMIC_FILENAME", executable.c_str(), 1);
    std::system((launcher + " " + exec_str).c_str());


    return 0;
}
