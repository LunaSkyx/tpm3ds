/* COM objects for the DirectX HLE.
 * A guest interface pointer is a 16-byte guest block: [vtbl][object id][iface id][0].
 * One host object can be seen through several interfaces (QueryInterface returns
 * a different guest pointer with another vtable). Vtable slots are HLE thunks
 * named "Iface::Method"; slots without an implementation stop the guest with
 * that name, so nothing is silently stubbed. */
#ifndef HLE_COM_H
#define HLE_COM_H

#include <stdint.h>
#include "core/cpu.h"
#include "core/hle/hle.h"

#define S_OK            0x00000000u
#define S_FALSE         0x00000001u
#define E_NOINTERFACE   0x80004002u
#define E_FAIL          0x80004005u
#define E_INVALIDARG    0x80070057u
#define E_OUTOFMEMORY   0x8007000eu
#define DDERR_GENERIC   E_FAIL
#define DDERR_NOTFOUND  0x887600ffu
#define DDERR_WASSTILLDRAWING 0x8876021cu
#define DDERR_UNSUPPORTED     0x80004001u

typedef struct {
    const char *name;     /* method name as in the SDK headers */
    hle_fn      fn;       /* NULL: not implemented yet (stops the guest when called) */
} com_method;

typedef struct com_iface {
    const char       *name;       /* "IDirectDraw4" */
    uint32_t          iid0;       /* first dword of the IID (all DirectX IIDs differ there) */
    const com_method *methods;    /* in vtable order, starting with QueryInterface */
    int               nmethods;
    gptr              vtbl;       /* built on first use */
} com_iface;

typedef void (*com_dtor)(void *data);

typedef struct {
    int         used;
    uint32_t    refs;
    const char *kind;             /* "surface", "ddraw", ... */
    void       *data;             /* host-side state */
    com_dtor    dtor;
    const com_iface *const *ifaces;   /* NULL-terminated list this object answers QI for */
    gptr        ptrs[8];          /* guest interface pointers, parallel to ifaces */
} com_obj;

/* Create an object; returns the guest pointer for ifaces[0]. */
gptr     com_create(const char *kind, void *data, com_dtor dtor, const com_iface *const *ifaces);
com_obj *com_get(gptr iface_ptr);                 /* NULL if not one of ours */
void    *com_data(gptr iface_ptr, const char *kind); /* NULL if not that kind */
gptr     com_ptr(com_obj *o, const com_iface *ifc);   /* interface pointer (made on demand) */
uint32_t com_addref(gptr p);
uint32_t com_release(gptr p);

/* IUnknown implementations usable in every method table */
void com_QueryInterface(cpu_t *c);
void com_AddRef(cpu_t *c);
void com_Release(cpu_t *c);
#define COM_IUNKNOWN {"QueryInterface", com_QueryInterface}, {"AddRef", com_AddRef}, {"Release", com_Release}

/* Register an interface's implemented methods as HLE functions (call from init). */
void com_register(com_iface *ifc);
void com_reset(void);

#endif
