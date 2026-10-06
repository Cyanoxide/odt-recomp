/*
 * Widescreen activation plugin. Widescreen is mod-owned on PSX: [video]
 * aspect_ratio is inert, so a trusted activation plugin is the only route.
 */

#include "mod_plugins.h"
#include "gpu.h"

static void odt_widescreen_activate(void) {
    (void)psx_mod_set_fixed_display_aspect(16u, 9u);
    /* O.D.T.'s HUD is flat-textured quads, which the generic sprite
     * un-squash paths never see. */
    gpu_ws_set_odt_hud_band(1);
}

PSX_MOD_CONSTRUCTOR(odt_register_widescreen_plugin) {
    (void)psx_mod_register_activation_plugin("odt.widescreen",
                                             odt_widescreen_activate);
}
