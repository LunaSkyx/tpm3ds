#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/gheap.h"
#include "core/hle/hle_com.h"
#include "pal/pal.h"

#define MAX_OBJS 2048
static com_obj g_objs[MAX_OBJS];

static int iface_index(const com_obj *o, const com_iface *ifc)
{
    for (int k = 0; o->ifaces[k]; k++) if (o->ifaces[k] == ifc) return k;
    return -1;
}

static gptr build_vtbl(com_iface *ifc)
{
    if (ifc->vtbl) return ifc->vtbl;
    gptr v = gheap_alloc(4u * (uint32_t)ifc->nmethods);
    for (int k = 0; k < ifc->nmethods; k++) {
        char name[96];
        snprintf(name, sizeof name, "%s::%s", ifc->name, ifc->methods[k].name);
        gmem_wr32(v + 4u * (uint32_t)k, hle_thunk_for("COM", name));
    }
    ifc->vtbl = v;
    return v;
}

gptr com_ptr(com_obj *o, const com_iface *ifc)
{
    int k = iface_index(o, ifc);
    if (k < 0 || k >= 8) return 0;
    if (!o->ptrs[k]) {
        gptr p = gheap_alloc(16);
        gmem_wr32(p, build_vtbl((com_iface *)ifc));
        gmem_wr32(p + 4, (uint32_t)(o - g_objs) + 1u);
        gmem_wr32(p + 8, (uint32_t)k);
        o->ptrs[k] = p;
    }
    return o->ptrs[k];
}

gptr com_create(const char *kind, void *data, com_dtor dtor, const com_iface *const *ifaces)
{
    for (int i = 0; i < MAX_OBJS; i++)
        if (!g_objs[i].used) {
            com_obj *o = &g_objs[i];
            memset(o, 0, sizeof *o);
            o->used = 1;
            o->refs = 1;
            o->kind = kind;
            o->data = data;
            o->dtor = dtor;
            o->ifaces = ifaces;
            return com_ptr(o, ifaces[0]);
        }
    pal_log("com: object table full");
    return 0;
}

com_obj *com_get(gptr p)
{
    if (!p || !gmem_ok(p, 16)) return NULL;
    uint32_t id = gmem_rd32(p + 4), k = gmem_rd32(p + 8);
    if (id == 0 || id > MAX_OBJS || k >= 8) return NULL;
    com_obj *o = &g_objs[id - 1];
    return (o->used && o->ptrs[k] == p) ? o : NULL;
}

void *com_data(gptr p, const char *kind)
{
    com_obj *o = com_get(p);
    return (o && !strcmp(o->kind, kind)) ? o->data : NULL;
}

uint32_t com_addref(gptr p)
{
    com_obj *o = com_get(p);
    return o ? ++o->refs : 0;
}

uint32_t com_release(gptr p)
{
    com_obj *o = com_get(p);
    if (!o) return 0;
    if (--o->refs) return o->refs;
    if (o->dtor) o->dtor(o->data);
    for (int k = 0; k < 8; k++) if (o->ptrs[k]) gheap_free(o->ptrs[k]);
    o->used = 0;
    return 0;
}

/* QueryInterface(this, riid, ppv) */
void com_QueryInterface(cpu_t *c)
{
    com_obj *o = com_get(ARG(0));
    uint32_t iid0 = gmem_rd32(ARG(1));
    gptr out = ARG(2);
    if (o)
        for (int k = 0; o->ifaces[k]; k++)
            if (o->ifaces[k]->iid0 == iid0) {
                gptr p = com_ptr(o, o->ifaces[k]);
                o->refs++;
                gmem_wr32(out, p);
                hle_ret(c, S_OK, 3);
                return;
            }
    pal_log("com: QueryInterface(%s, iid %08lx) -> E_NOINTERFACE", o ? o->kind : "?", (unsigned long)iid0);
    if (out) gmem_wr32(out, 0);
    hle_ret(c, E_NOINTERFACE, 3);
}

void com_AddRef(cpu_t *c)  { hle_ret(c, com_addref(ARG(0)), 1); }
void com_Release(cpu_t *c) { hle_ret(c, com_release(ARG(0)), 1); }

void com_register(com_iface *ifc)
{
    hle_def *defs = calloc((size_t)ifc->nmethods, sizeof *defs);
    int n = 0;
    for (int k = 0; k < ifc->nmethods; k++) {
        if (!ifc->methods[k].fn) continue;
        char *name = malloc(96);
        snprintf(name, 96, "%s::%s", ifc->name, ifc->methods[k].name);
        defs[n++] = (hle_def){"COM", name, ifc->methods[k].fn};
    }
    hle_register(defs, n);          /* hle keeps a copy of the entries; names stay allocated */
    free(defs);
    ifc->vtbl = 0;
}

void com_reset(void) { memset(g_objs, 0, sizeof g_objs); }
