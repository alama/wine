/*
 * Copyright 2011 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "initguid.h"

#include "vbscript.h"
#include "objsafe.h"
#include "mshtmhst.h"
#include "rpcproxy.h"
#include "vbscript_classes.h"
#include "vbsglobal.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(vbscript);

DEFINE_GUID(GUID_NULL,0,0,0,0,0,0,0,0,0,0,0);

static HINSTANCE vbscript_hinstance;

static ITypeLib *typelib;
static ITypeInfo *typeinfos[LAST_tid];

static REFIID tid_ids[] = {
#define XDIID(iface) &DIID_ ## iface,
TID_LIST
#undef XDIID
};

HRESULT get_typeinfo(tid_t tid, ITypeInfo **typeinfo)
{
    HRESULT hres;

    if (!typelib) {
        ITypeLib *tl;

        static const WCHAR vbscript_dll1W[] = {'v','b','s','c','r','i','p','t','.','d','l','l','\\','1',0};

        hres = LoadTypeLib(vbscript_dll1W, &tl);
        if(FAILED(hres)) {
            ERR("LoadRegTypeLib failed: %08x\n", hres);
            return hres;
        }

        if(InterlockedCompareExchangePointer((void**)&typelib, tl, NULL))
            ITypeLib_Release(tl);
    }

    if(!typeinfos[tid]) {
        ITypeInfo *ti;

        hres = ITypeLib_GetTypeInfoOfGuid(typelib, tid_ids[tid], &ti);
        if(FAILED(hres)) {
            ERR("GetTypeInfoOfGuid(%s) failed: %08x\n", debugstr_guid(tid_ids[tid]), hres);
            return hres;
        }

        if(InterlockedCompareExchangePointer((void**)(typeinfos+tid), ti, NULL))
            ITypeInfo_Release(ti);
    }

    *typeinfo = typeinfos[tid];
    return S_OK;
}

static void release_typelib(void)
{
    unsigned i;

    if(!typelib)
        return;

    for(i=0; i < sizeof(typeinfos)/sizeof(*typeinfos); i++) {
        if(typeinfos[i])
            ITypeInfo_Release(typeinfos[i]);
    }

    ITypeLib_Release(typelib);
}

const char *debugstr_variant(const VARIANT *v)
{
    if(!v)
        return "(null)";

    if(V_ISBYREF(v))
        return wine_dbg_sprintf("{V_BYREF -> %s}", debugstr_variant(V_BYREF(v)));

    switch(V_VT(v)) {
    case VT_EMPTY:
        return "{VT_EMPTY}";
    case VT_NULL:
        return "{VT_NULL}";
    case VT_I2:
        return wine_dbg_sprintf("{VT_I2: %d}", V_I2(v));
    case VT_I4:
        return wine_dbg_sprintf("{VT_I4: %d}", V_I4(v));
    case VT_UI4:
        return wine_dbg_sprintf("{VT_UI4: %u}", V_UI4(v));
    case VT_R8:
        return wine_dbg_sprintf("{VT_R8: %lf}", V_R8(v));
    case VT_BSTR:
        return wine_dbg_sprintf("{VT_BSTR: %s}", debugstr_w(V_BSTR(v)));
    case VT_DISPATCH:
        return wine_dbg_sprintf("{VT_DISPATCH: %p}", V_DISPATCH(v));
    case VT_BOOL:
        return wine_dbg_sprintf("{VT_BOOL: %x}", V_BOOL(v));
    default:
        return wine_dbg_sprintf("{vt %d}", V_VT(v));
    }
}

#define MIN_BLOCK_SIZE  128

static inline DWORD block_size(DWORD block)
{
    return MIN_BLOCK_SIZE << block;
}

void vbsheap_init(vbsheap_t *heap)
{
    memset(heap, 0, sizeof(*heap));
    list_init(&heap->custom_blocks);
}

void *vbsheap_alloc(vbsheap_t *heap, size_t size)
{
    struct list *list;
    void *tmp;

    size = (size+3)&~3;

    if(!heap->block_cnt) {
        if(!heap->blocks) {
            heap->blocks = heap_alloc(sizeof(void*));
            if(!heap->blocks)
                return NULL;
        }

        tmp = heap_alloc(block_size(0));
        if(!tmp)
            return NULL;

        heap->blocks[0] = tmp;
        heap->block_cnt = 1;
    }

    if(heap->offset + size <= block_size(heap->last_block)) {
        tmp = ((BYTE*)heap->blocks[heap->last_block])+heap->offset;
        heap->offset += size;
        return tmp;
    }

    if(size <= block_size(heap->last_block+1)) {
        if(heap->last_block+1 == heap->block_cnt) {
            tmp = heap_realloc(heap->blocks, (heap->block_cnt+1)*sizeof(void*));
            if(!tmp)
                return NULL;

            heap->blocks = tmp;
            heap->blocks[heap->block_cnt] = heap_alloc(block_size(heap->block_cnt));
            if(!heap->blocks[heap->block_cnt])
                return NULL;

            heap->block_cnt++;
        }

        heap->last_block++;
        heap->offset = size;
        return heap->blocks[heap->last_block];
    }

    list = heap_alloc(size + sizeof(struct list));
    if(!list)
        return NULL;

    list_add_head(&heap->custom_blocks, list);
    return list+1;
}

void vbsheap_free(vbsheap_t *heap)
{
    struct list *iter;
    DWORD i;

    while((iter = list_next(&heap->custom_blocks, &heap->custom_blocks))) {
        list_remove(iter);
        heap_free(iter);
    }

    for(i=0; i < heap->block_cnt; i++)
        heap_free(heap->blocks[i]);
    heap_free(heap->blocks);
}

static HRESULT WINAPI ClassFactory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    *ppv = NULL;

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        TRACE("(%p)->(IID_IUnknown %p)\n", iface, ppv);
        *ppv = iface;
    }else if(IsEqualGUID(&IID_IClassFactory, riid)) {
        TRACE("(%p)->(IID_IClassFactory %p)\n", iface, ppv);
        *ppv = iface;
    }

    if(*ppv) {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    FIXME("(%p)->(%s %p)\n", iface, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

static ULONG WINAPI ClassFactory_AddRef(IClassFactory *iface)
{
    TRACE("(%p)\n", iface);
    return 2;
}

static ULONG WINAPI ClassFactory_Release(IClassFactory *iface)
{
    TRACE("(%p)\n", iface);
    return 1;
}

static HRESULT WINAPI ClassFactory_LockServer(IClassFactory *iface, BOOL fLock)
{
    TRACE("(%p)->(%x)\n", iface, fLock);
    return S_OK;
}

static const IClassFactoryVtbl VBScriptFactoryVtbl = {
    ClassFactory_QueryInterface,
    ClassFactory_AddRef,
    ClassFactory_Release,
    VBScriptFactory_CreateInstance,
    ClassFactory_LockServer
};

static IClassFactory VBScriptFactory = { &VBScriptFactoryVtbl };

/******************************************************************
 *              DllMain (vbscript.@)
 */
BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpv)
{
    TRACE("(%p %d %p)\n", hInstDLL, fdwReason, lpv);

    switch(fdwReason)
    {
    case DLL_WINE_PREATTACH:
        return FALSE;  /* prefer native version */
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInstDLL);
        vbscript_hinstance = hInstDLL;
        break;
    case DLL_PROCESS_DETACH:
        release_typelib();
    }

    return TRUE;
}

/***********************************************************************
 *		DllGetClassObject	(vbscript.@)
 */
HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    if(IsEqualGUID(&CLSID_VBScript, rclsid)) {
        TRACE("(CLSID_VBScript %s %p)\n", debugstr_guid(riid), ppv);
        return IClassFactory_QueryInterface(&VBScriptFactory, riid, ppv);
    }

    FIXME("%s %s %p\n", debugstr_guid(rclsid), debugstr_guid(riid), ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

/***********************************************************************
 *          DllCanUnloadNow (vbscript.@)
 */
HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

/***********************************************************************
 *          DllRegisterServer (vbscript.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    TRACE("()\n");
    return __wine_register_resources(vbscript_hinstance);
}

/***********************************************************************
 *          DllUnregisterServer (vbscript.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    TRACE("()\n");
    return __wine_unregister_resources(vbscript_hinstance);
}
