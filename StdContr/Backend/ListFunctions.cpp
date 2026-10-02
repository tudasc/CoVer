#include <fstream>
#include <string>

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <cgraph.h>
#include <diagnostic-core.h>

static std::string list_file;

// Append all functions of this unit, defined or referenced (called, used through pointers,
// from initializers), in one write, as units may be compiled in parallel.
// Run at the end, as instrumentation inserts calls after the IPA passes
static void write_functions(void*, void*) {
    std::string buf;
    symtab_node* node;
    FOR_EACH_SYMBOL(node)
        if (TREE_CODE(node->decl) == FUNCTION_DECL) buf += std::string(IDENTIFIER_POINTER(DECL_ASSEMBLER_NAME(node->decl))) + "\n";
    if (buf.empty()) return;

    std::ofstream out(list_file, std::ios::app);
    if (!out) {
        error("cover_list_functions: cannot open %s", list_file.c_str());
        return;
    }
    out << buf;
}

void setup_listfuncs_pass(struct plugin_name_args* plugin_info, std::string out_file) {
    list_file = out_file;
    if (list_file.empty()) return;
    register_callback(plugin_info->base_name, PLUGIN_FINISH_UNIT, write_functions, NULL);
}
