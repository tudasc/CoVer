#include <gcc-plugin.h>
#include <diagnostic-core.h>
#include <plugin-version.h>
#include <string>

int plugin_is_GPL_compatible;

void setup_funcreplace_pass(struct plugin_name_args* plugin_info, std::string list_file);
void setup_annotdiscard_pass(struct plugin_name_args* plugin_info);
void setup_memregister_pass(struct plugin_name_args* plugin_info, bool is_runtime_tu);
void setup_meminstr_pass(struct plugin_name_args* plugin_info, bool is_runtime_tu);
void setup_runtime_isolation_pass(struct plugin_name_args* plugin_info, bool is_runtime_tu);
void setup_listfuncs_pass(struct plugin_name_args* plugin_info, std::string out_file);

int plugin_init(struct plugin_name_args* plugin_info, struct plugin_gcc_version* version) {
    if (!plugin_default_version_check(version, &gcc_version))
        return 1;

    // Parse Arguments
    std::string list_file;
    std::string used_list_file;
    bool is_runtime_tu = false;
    for (int i = 0; i < plugin_info->argc; i++) {
        if (!strcmp(plugin_info->argv[i].key, "list"))
            list_file = plugin_info->argv[i].value;
        // Where to append the functions this unit defines or references
        if (!strcmp(plugin_info->argv[i].key, "used-list"))
            used_list_file = plugin_info->argv[i].value;
        // Marks the generated unit holding the contract checks themselves
        if (!strcmp(plugin_info->argv[i].key, "runtime"))
            is_runtime_tu = true;
    }

    // Runs ahead of optimization so wrappers are inlined where useful
    setup_funcreplace_pass(plugin_info, list_file);

    // RUns ahead of visibility, keeps the analysis runtime from binding to instrumented code
    setup_runtime_isolation_pass(plugin_info, is_runtime_tu);

    // After optimization, in order of execution
    setup_meminstr_pass(plugin_info, is_runtime_tu);
    setup_memregister_pass(plugin_info, is_runtime_tu);
    setup_listfuncs_pass(plugin_info, used_list_file);

    // Register contract annotation handler
    setup_annotdiscard_pass(plugin_info);
    
    return 0;
}
