/*
 * Widescreen activation plugin.
 *
 * Widescreen is mod-owned on PSX: `[video] aspect_ratio` is inert and clamped
 * back to 4:3 by the runtime. A trusted activation plugin is the supported
 * route. The mod manifest binds a feature to this plugin id; PSX_MOD_CONSTRUCTOR
 * self-registers, so linking the file is enough.
 */

#include "mod_plugins.h"

static void odt_widescreen_activate(void) {
    (void)psx_mod_set_fixed_display_aspect(16u, 9u);
}

PSX_MOD_CONSTRUCTOR(odt_register_widescreen_plugin) {
    (void)psx_mod_register_activation_plugin("odt.widescreen",
                                             odt_widescreen_activate);
}
