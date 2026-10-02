/*
 * evo_hw.c - PS5 Pro ("Trinity") detection. See evo_hw.h.
 */
#include "evo_hw.h"
#include "evo_boot_log.h"

#include <stddef.h>
#include <stdint.h>

int sceKernelDlsym(int moduleHandle, const char *symbol, void **addrOut);
int sceKernelLoadStartModule(const char *name, size_t argc, const void *argv,
                             unsigned int flags, void *opt, int *res);

static int s_probed;
static int s_trinity_mode = EVO_HW_UNKNOWN;
static int s_authentic    = EVO_HW_UNKNOWN;

/* libkernel's module handle. 0x2001 is where the PS5 loader puts libkernel_sys
 * in every process; asking the loader for the already-loaded module by name is
 * the fallback. A wrong handle is harmless - Dlsym fails and we try the next.
 */
static int try_handle(int h, const char *name, const char *nid, void **out)
{
    int rc_name = sceKernelDlsym(h, name, out);
    if (rc_name == 0 && *out)
        return 0;
    int rc_nid = sceKernelDlsym(h, nid, out);
    if (rc_nid == 0 && *out)
        return 0;
    /* First run on hardware reported "?" for both queries; say why. */
    evo_boot_log("hw: dlsym %s handle=%#x rc_name=%#x rc_nid=%#x",
                 name, (unsigned)h, (unsigned)rc_name, (unsigned)rc_nid);
    *out = NULL;
    return -1;
}

static int resolve(const char *name, const char *nid, void **out)
{
    static const int fixed[] = { 0x2001, 0x2 };
    *out = NULL;
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i)
        if (try_handle(fixed[i], name, nid, out) == 0)
            return 0;
    static const char *const mods[] = { "libkernel_sys.sprx", "libkernel.sprx" };
    for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); ++i) {
        int res = 0;
        int h = sceKernelLoadStartModule(mods[i], 0, NULL, 0, NULL, &res);
        if (h <= 0) {
            evo_boot_log("hw: load %s -> %#x", mods[i], (unsigned)h);
            continue;
        }
        if (try_handle(h, name, nid, out) == 0)
            return 0;
    }
    return -1;
}

/* The queries are declared as returning a bool, so only the low byte is
 * defined; an SCE error (high bit set) means the answer is unknown. */
static int query(const char *name, const char *nid)
{
    void *fn = NULL;
    if (resolve(name, nid, &fn) != 0)
        return EVO_HW_UNKNOWN;
    const int raw = ((int (*)(void))fn)();
    if ((uint32_t)raw & 0x80000000u)
        return EVO_HW_UNKNOWN;
    return ((uint32_t)raw & 0xffu) ? 1 : 0;
}

static char tri(int v)
{
    return v == EVO_HW_UNKNOWN ? '?' : (v ? '1' : '0');
}

void evo_hw_probe(void)
{
    if (s_probed)
        return;
    s_probed = 1;
    s_trinity_mode = query("sceKernelHasTrinityMode", "yu17wG8L5FI");
    s_authentic    = query("sceKernelIsAuthenticTrinity", "X0HkB92+NRE");
    const int pro = evo_hw_is_ps5_pro();
    evo_boot_log("hw: ps5 pro=%c trinity_mode=%c authentic=%c",
                 (s_trinity_mode == EVO_HW_UNKNOWN && s_authentic == EVO_HW_UNKNOWN)
                     ? '?' : (pro ? '1' : '0'),
                 tri(s_trinity_mode), tri(s_authentic));
}

/* Authentic Trinity hardware is a Pro. When that query alone is missing, a
 * title running in Pro mode can only be on a Pro. */
int evo_hw_is_ps5_pro(void)
{
    if (s_authentic != EVO_HW_UNKNOWN)
        return s_authentic == 1;
    return s_trinity_mode == 1;
}

int evo_hw_trinity_mode(void)      { return s_trinity_mode; }
int evo_hw_authentic_trinity(void) { return s_authentic; }

int evo_hw_model_known(void)
{
    return s_trinity_mode != EVO_HW_UNKNOWN || s_authentic != EVO_HW_UNKNOWN;
}

const char *evo_hw_model_name(void)
{
    if (evo_hw_is_ps5_pro())
        return "PS5 Pro";
    return evo_hw_model_known() ? "PS5" : "PS5 model unknown";
}
