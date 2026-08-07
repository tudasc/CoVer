/* rewrite_calls.c
 * Build: gcc -I`gcc -print-file-name=plugin`/include -fPIC -shared \
 *            rewrite_calls.c -o rewrite_calls.so
 * Use:   gcc -fplugin=./rewrite_calls.so \
 *            -fplugin-arg-rewrite_calls-list=funcs.txt file.c
 *
 * funcs.txt: one function name per line. Blank lines and lines
 * starting with '#' are ignored. Each call to <name> becomes a
 * call to wrap_<name>.
 */

#include <string>
#include <set>
#include <fstream>

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <tree-pass.h>
#include <context.h>
#include <function.h>
#include <basic-block.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <diagnostic-core.h>
#include <gimple-ssa.h>
#include <tree-cfg.h>
#include <cgraph.h>
#include <stringpool.h>

// Names of functions whose calls should be rewritten.
static std::set<std::string> targets;

// Synthesize an extern decl with same type as old_fndecl but name asm_name,
// and replace the call
static tree get_replacement_decl(const char* asm_name, tree old_fndecl) {
    tree asm_id = get_identifier(asm_name);

    /* Synthesize an extern decl. Give it a plausible source name, but
       pin the assembler name so GCC does not mangle it. */
    tree decl = build_decl(UNKNOWN_LOCATION, FUNCTION_DECL, get_identifier(asm_name), TREE_TYPE(old_fndecl));
    TREE_PUBLIC(decl) = 1;
    DECL_EXTERNAL(decl) = 1;
    TREE_USED(decl) = 1;
    SET_DECL_ASSEMBLER_NAME(decl, asm_id);
    return decl;
}

namespace {

const pass_data rewrite_pass_data = {
    GIMPLE_PASS,
    "rewrite_calls", /* name */
    OPTGROUP_NONE,   /* optinfo_flags */
    TV_NONE,         /* tv_id */
    PROP_gimple_any, /* properties_required */
    0,               /* properties_provided */
    0,               /* properties_destroyed */
    0,               /* todo_flags_start */
    TODO_update_ssa, /* todo_flags_finish */
};

struct rewrite_pass : gimple_opt_pass {
    rewrite_pass(gcc::context* ctxt) : gimple_opt_pass(rewrite_pass_data, ctxt) {}

    unsigned int execute(function* fun) override {
        /* Skip functions that are themselves wrappers. */
        tree fndecl = fun->decl;
        tree asm_id = DECL_ASSEMBLER_NAME(fndecl);
        std::string cur = IDENTIFIER_POINTER(asm_id);
        if (cur.contains("CoVer_Wrapper_"))
            return 0;

        basic_block bb;

        FOR_EACH_BB_FN(bb, fun) {
            for (gimple_stmt_iterator gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
                gimple* stmt = gsi_stmt(gsi);
                if (!is_gimple_call(stmt))
                    continue;

                tree callee = gimple_call_fndecl(stmt);
                if (!callee)
                    continue;

                /* Match on the mangled (assembler) name. */
                tree asm_id = DECL_ASSEMBLER_NAME(callee);
                if (!asm_id)
                    continue;
                const char* mangled = IDENTIFIER_POINTER(asm_id);

                if (targets.find(mangled) == targets.end())
                    continue;

                std::string newname = std::string("CoVer_Wrapper_") + mangled;
                tree repl = get_replacement_decl(newname.c_str(), callee);
                gimple_call_set_fndecl(as_a<gcall*>(stmt), repl);
                update_stmt(stmt);
            }
        }
        return 0;
    }
};

} // namespace

static void load_targets(std::string path) {
    std::ifstream in(path);
    if (!in) {
        error("rewrite_calls: cannot open function list %s", path.c_str());
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        /* Trim leading/trailing whitespace. */
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        std::string name = line.substr(b, e - b + 1);
        if (name.empty() || name[0] == '#')
            continue;
        targets.insert(name);
    }
}

void setup_funcreplace_pass(struct plugin_name_args* plugin_info, std::string list_file) {
    if (list_file.empty()) return; // Nothing to do
    load_targets(list_file);

    struct register_pass_info pass_info;
    pass_info.pass = new rewrite_pass(g);
    pass_info.reference_pass_name = "cfg";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;

    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
}
