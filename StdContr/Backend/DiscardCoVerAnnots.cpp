#include "gcc-plugin.h"
#include "plugin-version.h"
#include "tree.h"

/* Handler invoked when GCC applies the attribute to a decl/type.
   Returning without setting *no_add_attrs = true keeps the attribute
   attached to the tree, so your later pass can read it back. */
static tree handle_annotate_attribute(tree* node ATTRIBUTE_UNUSED, tree name ATTRIBUTE_UNUSED, tree args ATTRIBUTE_UNUSED, int flags ATTRIBUTE_UNUSED,
                                      bool* no_add_attrs) {
    *no_add_attrs = true;
    return NULL_TREE;
}

static const struct attribute_spec annotate_attr = {
    .name = "annotate",
    .min_length = 1,
    .max_length = -1,
    .decl_required = false,
    .type_required = false,
    .function_type_required = false,
    .affects_type_identity = false,
    .handler = handle_annotate_attribute,
    .exclude = NULL,
};

static void register_annotate_attribute(void* event_data ATTRIBUTE_UNUSED, void* user_data ATTRIBUTE_UNUSED) { register_attribute(&annotate_attr); }

void setup_annotdiscard_pass(struct plugin_name_args* plugin_info) {
    /* Attributes must be registered during PLUGIN_ATTRIBUTES. */
    register_callback(plugin_info->base_name, PLUGIN_ATTRIBUTES, register_annotate_attribute, NULL);
}
