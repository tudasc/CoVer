#include <gcc-plugin.h>
#include <diagnostic-core.h>
#include <plugin-version.h>
#include <string>

int plugin_is_GPL_compatible;

void setup_funcreplace_pass(struct plugin_name_args* plugin_info, std::string list_file);
void setup_annotdiscard_pass(struct plugin_name_args* plugin_info);
void setup_memregister_pass(struct plugin_name_args* plugin_info);

int plugin_init(struct plugin_name_args* plugin_info, struct plugin_gcc_version* version) {
    if (!plugin_default_version_check(version, &gcc_version))
        return 1;

    // Parse Arguments
    std::string list_file;
    for (int i = 0; i < plugin_info->argc; i++)
        if (!strcmp(plugin_info->argv[i].key, "list"))
            list_file = plugin_info->argv[i].value;

    // Setup passes in reverse order that they are run
    setup_funcreplace_pass(plugin_info, list_file);
    setup_memregister_pass(plugin_info); // Must be registered after the funcreplace pass, which it references
    setup_annotdiscard_pass(plugin_info);
    
    return 0;
}
