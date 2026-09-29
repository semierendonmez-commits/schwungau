/* <link.h> for macOS.
 *
 * The Schwung chain host uses one piece of it: after loading a synth it logs
 * the image's load address, read with the GNU extension
 * dlinfo(handle, RTLD_DI_LINKMAP, &lm) -> lm->l_addr. macOS has neither
 * dlinfo nor struct link_map; the same fact is available from dladdr() on any
 * symbol of the image. Only the members the chain host reads are provided.
 */
#pragma once
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>

struct link_map {
    uintptr_t        l_addr;   /* load address of the image */
    const char      *l_name;   /* path of the image */
    void            *l_ld;
    struct link_map *l_next, *l_prev;
};

#ifndef RTLD_DI_LINKMAP
#define RTLD_DI_LINKMAP 2
#endif

static inline int sw_dlinfo(void *handle, int request, void *arg) {
    if (request != RTLD_DI_LINKMAP || !handle || !arg) return -1;
    /* Any exported symbol locates the image; Schwung modules export one of
     * these entry points. */
    static const char *const entry[] = {
        "move_plugin_init_v2", "move_audio_fx_init_v2", "move_midi_fx_init",
        "move_plugin_init", "move_audio_fx_init", NULL };
    for (int i = 0; entry[i]; ++i) {
        void *sym = dlsym(handle, entry[i]);
        Dl_info info;
        if (sym && dladdr(sym, &info) && info.dli_fbase) {
            /* One slot is enough: the chain host reads it immediately, under
             * its own lock, and only to print it. */
            static struct link_map lm;
            lm.l_addr = (uintptr_t)info.dli_fbase;
            lm.l_name = info.dli_fname;
            lm.l_ld = NULL; lm.l_next = lm.l_prev = NULL;
            *(struct link_map **)arg = &lm;
            return 0;
        }
    }
    return -1;
}
#define dlinfo(h, r, a) sw_dlinfo((h), (r), (a))
