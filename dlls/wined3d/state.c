/*
 * Direct3D state management
 *
 * Copyright 2002 Lionel Ulmer
 * Copyright 2002-2005 Jason Edmeades
 * Copyright 2003-2004 Raphael Junqueira
 * Copyright 2004 Christian Costa
 * Copyright 2005 Oliver Stieber
 * Copyright 2006 Henri Verbeet
 * Copyright 2006-2008 Stefan Dösinger for CodeWeavers
 * Copyright 2009-2011 Henri Verbeet for CodeWeavers
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

#include "config.h"
#include <stdio.h>
#ifdef HAVE_FLOAT_H
# include <float.h>
#endif
#include "wined3d_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);
WINE_DECLARE_DEBUG_CHANNEL(d3d_shader);

/* GL locking for state handlers is done by the caller. */

static void state_undefined(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    static BOOL once = FALSE;
    if (state_id)
        ERR("Undefined state %s\n", debug_d3dstate(state_id));
    else if (!once)
    {
        ERR("Undefined state.\n");
        once = TRUE;
    }
}

static void state_nop(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    TRACE("%s: nop in current pipe config.\n", debug_d3dstate(state_id));
}

static void state_fillmode(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    WINED3DFILLMODE mode = state->render_states[WINED3D_RS_FILLMODE];

    switch (mode)
    {
        case WINED3DFILL_POINT:
            glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
            checkGLcall("glPolygonMode(GL_FRONT_AND_BACK, GL_POINT)");
            break;
        case WINED3DFILL_WIREFRAME:
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            checkGLcall("glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)");
            break;
        case WINED3DFILL_SOLID:
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            checkGLcall("glPolygonMode(GL_FRONT_AND_BACK, GL_FILL)");
            break;
        default:
            FIXME("Unrecognized WINED3D_RS_FILLMODE %d.\n", mode);
    }
}

static void state_lighting(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    /* Lighting is not enabled if transformed vertices are drawn, but lighting
     * does not affect the stream sources, so it is not grouped for
     * performance reasons. This state reads the decoded vertex declaration,
     * so if it is dirty don't do anything. The vertex declaration applying
     * function calls this function for updating. */
    if (isStateDirty(context, STATE_VDECL))
        return;

    if (state->render_states[WINED3D_RS_LIGHTING]
            && !context->swapchain->device->strided_streams.position_transformed)
    {
        glEnable(GL_LIGHTING);
        checkGLcall("glEnable GL_LIGHTING");
    } else {
        glDisable(GL_LIGHTING);
        checkGLcall("glDisable GL_LIGHTING");
    }
}

static void state_zenable(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    /* No z test without depth stencil buffers */
    if (!state->fb->depth_stencil)
    {
        TRACE("No Z buffer - disabling depth test\n");
        glDisable(GL_DEPTH_TEST); /* This also disables z writing in gl */
        checkGLcall("glDisable GL_DEPTH_TEST");
        return;
    }

    switch (state->render_states[WINED3D_RS_ZENABLE])
    {
        case WINED3D_ZB_FALSE:
            glDisable(GL_DEPTH_TEST);
            checkGLcall("glDisable GL_DEPTH_TEST");
            break;
        case WINED3D_ZB_TRUE:
            glEnable(GL_DEPTH_TEST);
            checkGLcall("glEnable GL_DEPTH_TEST");
            break;
        case WINED3D_ZB_USEW:
            glEnable(GL_DEPTH_TEST);
            checkGLcall("glEnable GL_DEPTH_TEST");
            FIXME("W buffer is not well handled\n");
            break;
        default:
            FIXME("Unrecognized depth buffer type %#x.\n",
                    state->render_states[WINED3D_RS_ZENABLE]);
    }
}

static void state_cullmode(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    /* glFrontFace() is set in context.c at context init and on an
     * offscreen / onscreen rendering switch. */
    switch (state->render_states[WINED3D_RS_CULLMODE])
    {
        case WINED3DCULL_NONE:
            glDisable(GL_CULL_FACE);
            checkGLcall("glDisable GL_CULL_FACE");
            break;
        case WINED3DCULL_CW:
            glEnable(GL_CULL_FACE);
            checkGLcall("glEnable GL_CULL_FACE");
            glCullFace(GL_FRONT);
            checkGLcall("glCullFace(GL_FRONT)");
            break;
        case WINED3DCULL_CCW:
            glEnable(GL_CULL_FACE);
            checkGLcall("glEnable GL_CULL_FACE");
            glCullFace(GL_BACK);
            checkGLcall("glCullFace(GL_BACK)");
            break;
        default:
            FIXME("Unrecognized/Unhandled WINED3DCULL value %#x.\n",
                    state->render_states[WINED3D_RS_CULLMODE]);
    }
}

static void state_shademode(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    switch (state->render_states[WINED3D_RS_SHADEMODE])
    {
        case WINED3DSHADE_FLAT:
            glShadeModel(GL_FLAT);
            checkGLcall("glShadeModel(GL_FLAT)");
            break;
        case WINED3DSHADE_GOURAUD:
            glShadeModel(GL_SMOOTH);
            checkGLcall("glShadeModel(GL_SMOOTH)");
            break;
        case WINED3DSHADE_PHONG:
            FIXME("WINED3DSHADE_PHONG isn't supported\n");
            break;
        default:
            FIXME("Unrecognized/Unhandled WINED3DSHADEMODE value %#x.\n",
                    state->render_states[WINED3D_RS_SHADEMODE]);
    }
}

static void state_ditherenable(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_DITHERENABLE])
    {
        glEnable(GL_DITHER);
        checkGLcall("glEnable GL_DITHER");
    }
    else
    {
        glDisable(GL_DITHER);
        checkGLcall("glDisable GL_DITHER");
    }
}

static void state_zwritenable(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    /* TODO: Test if in d3d z writing is enabled even if ZENABLE is off.
     * If yes, this has to be merged with ZENABLE and ZFUNC. */
    if (state->render_states[WINED3D_RS_ZWRITEENABLE])
    {
        glDepthMask(1);
        checkGLcall("glDepthMask(1)");
    }
    else
    {
        glDepthMask(0);
        checkGLcall("glDepthMask(0)");
    }
}

static GLenum gl_compare_func(enum wined3d_cmp_func f)
{
    switch (f)
    {
        case WINED3D_CMP_NEVER:
            return GL_NEVER;
        case WINED3D_CMP_LESS:
            return GL_LESS;
        case WINED3D_CMP_EQUAL:
            return GL_EQUAL;
        case WINED3D_CMP_LESSEQUAL:
            return GL_LEQUAL;
        case WINED3D_CMP_GREATER:
            return GL_GREATER;
        case WINED3D_CMP_NOTEQUAL:
            return GL_NOTEQUAL;
        case WINED3D_CMP_GREATEREQUAL:
            return GL_GEQUAL;
        case WINED3D_CMP_ALWAYS:
            return GL_ALWAYS;
        default:
            FIXME("Unrecognized compare function %#x.\n", f);
            return GL_NONE;
    }
}

static void state_zfunc(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    GLenum depth_func = gl_compare_func(state->render_states[WINED3D_RS_ZFUNC]);

    if (!depth_func) return;

    if (depth_func == GL_EQUAL || depth_func == GL_NOTEQUAL)
    {
        static BOOL once;
        /* There are a few issues with this: First, our inability to
         * select a proper Z depth, most of the time we're stuck with
         * D24S8, even if the app selects D32 or D16. There seem to be
         * some other precision problems which have to be debugged to
         * make NOTEQUAL and EQUAL work properly. */
        if (!once)
        {
            once = TRUE;
            FIXME("D3DCMP_NOTEQUAL and D3DCMP_EQUAL do not work correctly yet.\n");
        }
    }

    glDepthFunc(depth_func);
    checkGLcall("glDepthFunc");
}

static void state_ambient(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    float col[4];

    D3DCOLORTOGLFLOAT4(state->render_states[WINED3D_RS_AMBIENT], col);
    TRACE("Setting ambient to (%f,%f,%f,%f)\n", col[0], col[1], col[2], col[3]);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, col);
    checkGLcall("glLightModel for MODEL_AMBIENT");
}

static void state_blendop_w(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    WARN("Unsupported in local OpenGL implementation: glBlendEquation\n");
}

static GLenum gl_blend_op(enum wined3d_blend_op op)
{
    switch (op)
    {
        case WINED3D_BLEND_OP_ADD:
            return GL_FUNC_ADD_EXT;
        case WINED3D_BLEND_OP_SUBTRACT:
            return GL_FUNC_SUBTRACT_EXT;
        case WINED3D_BLEND_OP_REVSUBTRACT:
            return GL_FUNC_REVERSE_SUBTRACT_EXT;
        case WINED3D_BLEND_OP_MIN:
            return GL_MIN_EXT;
        case WINED3D_BLEND_OP_MAX:
            return GL_MAX_EXT;
        default:
            FIXME("Unhandled blend op %#x.\n", op);
            return GL_NONE;
    }
}

static void state_blendop(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    GLenum blend_equation_alpha = GL_FUNC_ADD_EXT;
    GLenum blend_equation = GL_FUNC_ADD_EXT;

    /* BLENDOPALPHA requires GL_EXT_blend_equation_separate, so make sure it is around */
    if (state->render_states[WINED3D_RS_BLENDOPALPHA]
            && !gl_info->supported[EXT_BLEND_EQUATION_SEPARATE])
    {
        WARN("Unsupported in local OpenGL implementation: glBlendEquationSeparateEXT\n");
        return;
    }

    blend_equation = gl_blend_op(state->render_states[WINED3D_RS_BLENDOP]);
    blend_equation_alpha = gl_blend_op(state->render_states[WINED3D_RS_BLENDOPALPHA]);
    TRACE("blend_equation %#x, blend_equation_alpha %#x.\n", blend_equation, blend_equation_alpha);

    if (state->render_states[WINED3D_RS_SEPARATEALPHABLENDENABLE])
    {
        GL_EXTCALL(glBlendEquationSeparateEXT(blend_equation, blend_equation_alpha));
        checkGLcall("glBlendEquationSeparateEXT");
    }
    else
    {
        GL_EXTCALL(glBlendEquationEXT(blend_equation));
        checkGLcall("glBlendEquation");
    }
}

static GLenum gl_blend_factor(enum wined3d_blend factor, const struct wined3d_format *dst_format)
{
    switch (factor)
    {
        case WINED3D_BLEND_ZERO:
            return GL_ZERO;
        case WINED3D_BLEND_ONE:
            return GL_ONE;
        case WINED3D_BLEND_SRCCOLOR:
            return GL_SRC_COLOR;
        case WINED3D_BLEND_INVSRCCOLOR:
            return GL_ONE_MINUS_SRC_COLOR;
        case WINED3D_BLEND_SRCALPHA:
            return GL_SRC_ALPHA;
        case WINED3D_BLEND_INVSRCALPHA:
            return GL_ONE_MINUS_SRC_ALPHA;
        case WINED3D_BLEND_DESTCOLOR:
            return GL_DST_COLOR;
        case WINED3D_BLEND_INVDESTCOLOR:
            return GL_ONE_MINUS_DST_COLOR;
        /* To compensate for the lack of format switching with backbuffer
         * offscreen rendering, and with onscreen rendering, we modify the
         * alpha test parameters for (INV)DESTALPHA if the render target
         * doesn't support alpha blending. A nonexistent alpha channel
         * returns 1.0, so WINED3D_BLEND_DESTALPHA becomes GL_ONE, and
         * WINED3D_BLEND_INVDESTALPHA becomes GL_ZERO. */
        case WINED3D_BLEND_DESTALPHA:
            return dst_format->alpha_mask ? GL_DST_ALPHA : GL_ONE;
        case WINED3D_BLEND_INVDESTALPHA:
            return dst_format->alpha_mask ? GL_ONE_MINUS_DST_ALPHA : GL_ZERO;
        case WINED3D_BLEND_SRCALPHASAT:
            return GL_SRC_ALPHA_SATURATE;
        case WINED3D_BLEND_BLENDFACTOR:
            return GL_CONSTANT_COLOR_EXT;
        case WINED3D_BLEND_INVBLENDFACTOR:
            return GL_ONE_MINUS_CONSTANT_COLOR_EXT;
        default:
            FIXME("Unhandled blend factor %#x.\n", factor);
            return GL_NONE;
    }
}

static void state_blend(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_surface *target = state->fb->render_targets[0];
    const struct wined3d_gl_info *gl_info = context->gl_info;
    GLenum srcBlend, dstBlend;
    enum wined3d_blend d3d_blend;

    /* According to the red book, GL_LINE_SMOOTH needs GL_BLEND with specific
     * blending parameters to work. */
    if (state->render_states[WINED3D_RS_ALPHABLENDENABLE]
            || state->render_states[WINED3D_RS_EDGEANTIALIAS]
            || state->render_states[WINED3D_RS_ANTIALIASEDLINEENABLE])
    {
        /* Disable blending in all cases even without pixelshaders.
         * With blending on we could face a big performance penalty.
         * The d3d9 visual test confirms the behavior. */
        if (context->render_offscreen
                && !(target->resource.format->flags & WINED3DFMT_FLAG_POSTPIXELSHADER_BLENDING))
        {
            glDisable(GL_BLEND);
            checkGLcall("glDisable GL_BLEND");
            return;
        } else {
            glEnable(GL_BLEND);
            checkGLcall("glEnable GL_BLEND");
        }
    } else {
        glDisable(GL_BLEND);
        checkGLcall("glDisable GL_BLEND");
        /* Nothing more to do - get out */
        return;
    };

    /* WINED3D_BLEND_BOTHSRCALPHA and WINED3D_BLEND_BOTHINVSRCALPHA are legacy
     * source blending values which are still valid up to d3d9. They should
     * not occur as dest blend values. */
    d3d_blend = state->render_states[WINED3D_RS_SRCBLEND];
    if (d3d_blend == WINED3D_BLEND_BOTHSRCALPHA)
    {
        srcBlend = GL_SRC_ALPHA;
        dstBlend = GL_ONE_MINUS_SRC_ALPHA;
    }
    else if (d3d_blend == WINED3D_BLEND_BOTHINVSRCALPHA)
    {
        srcBlend = GL_ONE_MINUS_SRC_ALPHA;
        dstBlend = GL_SRC_ALPHA;
    }
    else
    {
        srcBlend = gl_blend_factor(d3d_blend, target->resource.format);
        dstBlend = gl_blend_factor(state->render_states[WINED3D_RS_DESTBLEND],
                target->resource.format);
    }

    if (state->render_states[WINED3D_RS_EDGEANTIALIAS]
            || state->render_states[WINED3D_RS_ANTIALIASEDLINEENABLE])
    {
        glEnable(GL_LINE_SMOOTH);
        checkGLcall("glEnable(GL_LINE_SMOOTH)");
        if(srcBlend != GL_SRC_ALPHA) {
            WARN("WINED3D_RS_EDGEANTIALIAS enabled, but unexpected src blending param\n");
        }
        if(dstBlend != GL_ONE_MINUS_SRC_ALPHA && dstBlend != GL_ONE) {
            WARN("WINED3D_RS_EDGEANTIALIAS enabled, but unexpected dst blending param\n");
        }
    } else {
        glDisable(GL_LINE_SMOOTH);
        checkGLcall("glDisable(GL_LINE_SMOOTH)");
    }

    /* Re-apply BLENDOP(ALPHA) because of a possible SEPARATEALPHABLENDENABLE change */
    if (!isStateDirty(context, STATE_RENDER(WINED3D_RS_BLENDOP)))
        state_blendop(context, state, STATE_RENDER(WINED3D_RS_BLENDOPALPHA));

    if (state->render_states[WINED3D_RS_SEPARATEALPHABLENDENABLE])
    {
        GLenum srcBlendAlpha, dstBlendAlpha;

        /* Separate alpha blending requires GL_EXT_blend_function_separate, so make sure it is around */
        if (!context->gl_info->supported[EXT_BLEND_FUNC_SEPARATE])
        {
            WARN("Unsupported in local OpenGL implementation: glBlendFuncSeparateEXT\n");
            return;
        }

        /* WINED3D_BLEND_BOTHSRCALPHA and WINED3D_BLEND_BOTHINVSRCALPHA are legacy
         * source blending values which are still valid up to d3d9. They should
         * not occur as dest blend values. */
        d3d_blend = state->render_states[WINED3D_RS_SRCBLENDALPHA];
        if (d3d_blend == WINED3D_BLEND_BOTHSRCALPHA)
        {
            srcBlendAlpha = GL_SRC_ALPHA;
            dstBlendAlpha = GL_ONE_MINUS_SRC_ALPHA;
        }
        else if (d3d_blend == WINED3D_BLEND_BOTHINVSRCALPHA)
        {
            srcBlendAlpha = GL_ONE_MINUS_SRC_ALPHA;
            dstBlendAlpha = GL_SRC_ALPHA;
        }
        else
        {
            srcBlendAlpha = gl_blend_factor(d3d_blend, target->resource.format);
            dstBlendAlpha = gl_blend_factor(state->render_states[WINED3D_RS_DESTBLENDALPHA],
                    target->resource.format);
        }

        GL_EXTCALL(glBlendFuncSeparateEXT(srcBlend, dstBlend, srcBlendAlpha, dstBlendAlpha));
        checkGLcall("glBlendFuncSeparateEXT");
    } else {
        TRACE("glBlendFunc src=%x, dst=%x\n", srcBlend, dstBlend);
        glBlendFunc(srcBlend, dstBlend);
        checkGLcall("glBlendFunc");
    }

    /* Colorkey fixup for stage 0 alphaop depends on
     * WINED3D_RS_ALPHABLENDENABLE state, so it may need updating. */
    if (state->render_states[WINED3D_RS_COLORKEYENABLE])
        context_apply_state(context, state, STATE_TEXTURESTAGE(0, WINED3DTSS_ALPHAOP));
}

static void state_blendfactor_w(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    WARN("Unsupported in local OpenGL implementation: glBlendColorEXT\n");
}

static void state_blendfactor(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    float col[4];

    TRACE("Setting blend factor to %#x.\n", state->render_states[WINED3D_RS_BLENDFACTOR]);

    D3DCOLORTOGLFLOAT4(state->render_states[WINED3D_RS_BLENDFACTOR], col);
    GL_EXTCALL(glBlendColorEXT (col[0],col[1],col[2],col[3]));
    checkGLcall("glBlendColor");
}

static void state_alpha(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    int glParm = 0;
    float ref;
    BOOL enable_ckey = FALSE;

    TRACE("context %p, state %p, state_id %#x.\n", context, state, state_id);

    /* Find out if the texture on the first stage has a ckey set
     * The alpha state func reads the texture settings, even though alpha and texture are not grouped
     * together. This is to avoid making a huge alpha+texture+texture stage+ckey block due to the hardly
     * used WINED3D_RS_COLORKEYENABLE state(which is d3d <= 3 only). The texture function will call alpha
     * in case it finds some texture+colorkeyenable combination which needs extra care.
     */
    if (state->textures[0])
    {
        struct wined3d_texture *texture = state->textures[0];
        GLenum texture_dimensions = texture->target;

        if (texture_dimensions == GL_TEXTURE_2D || texture_dimensions == GL_TEXTURE_RECTANGLE_ARB)
        {
            struct wined3d_surface *surf = surface_from_resource(texture->sub_resources[0]);

            if (surf->CKeyFlags & WINEDDSD_CKSRCBLT)
            {
                /* The surface conversion does not do color keying conversion for surfaces that have an alpha
                 * channel on their own. Likewise, the alpha test shouldn't be set up for color keying if the
                 * surface has alpha bits */
                if (!surf->resource.format->alpha_mask) enable_ckey = TRUE;
            }
        }
    }

    if (enable_ckey || context->last_was_ckey)
        context_apply_state(context, state, STATE_TEXTURESTAGE(0, WINED3DTSS_ALPHAOP));
    context->last_was_ckey = enable_ckey;

    if (state->render_states[WINED3D_RS_ALPHATESTENABLE]
            || (state->render_states[WINED3D_RS_COLORKEYENABLE] && enable_ckey))
    {
        glEnable(GL_ALPHA_TEST);
        checkGLcall("glEnable GL_ALPHA_TEST");
    } else {
        glDisable(GL_ALPHA_TEST);
        checkGLcall("glDisable GL_ALPHA_TEST");
        /* Alpha test is disabled, don't bother setting the params - it will happen on the next
         * enable call
         */
        return;
    }

    if (state->render_states[WINED3D_RS_COLORKEYENABLE] && enable_ckey)
    {
        glParm = GL_NOTEQUAL;
        ref = 0.0f;
    }
    else
    {
        ref = ((float)state->render_states[WINED3D_RS_ALPHAREF]) / 255.0f;
        glParm = gl_compare_func(state->render_states[WINED3D_RS_ALPHAFUNC]);
    }
    if(glParm) {
        glAlphaFunc(glParm, ref);
        checkGLcall("glAlphaFunc");
    }
}

static void shaderconstant(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_device *device = context->swapchain->device;

    /* Vertex and pixel shader states will call a shader upload, don't do
     * anything as long one of them has an update pending. */
    if (isStateDirty(context, STATE_VDECL)
            || isStateDirty(context, STATE_PIXELSHADER))
       return;

    device->shader_backend->shader_load_constants(context, use_ps(state), use_vs(state));
}

static void state_clipping(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD enable  = 0xFFFFFFFF;
    DWORD disable = 0x00000000;

    if (use_vs(state))
    {
        const struct wined3d_device *device = context->swapchain->device;

        if (!device->vs_clipping)
        {
            /* The spec says that opengl clipping planes are disabled when using shaders. Direct3D planes aren't,
             * so that is an issue. The MacOS ATI driver keeps clipping planes activated with shaders in some
             * conditions I got sick of tracking down. The shader state handler disables all clip planes because
             * of that - don't do anything here and keep them disabled
             */
            if (state->render_states[WINED3D_RS_CLIPPLANEENABLE])
            {
                static BOOL warned = FALSE;
                if(!warned) {
                    FIXME("Clipping not supported with vertex shaders\n");
                    warned = TRUE;
                }
            }
            return;
        }

        /* glEnable(GL_CLIP_PLANEx) doesn't apply to vertex shaders. The enabled / disabled planes are
         * hardcoded into the shader. Update the shader to update the enabled clipplanes */
        if (!isStateDirty(context, context->state_table[STATE_VSHADER].representative))
        {
            device->shader_backend->shader_select(context, use_ps(state), TRUE);
            if (!isStateDirty(context, STATE_VERTEXSHADERCONSTANT))
                shaderconstant(context, state, STATE_VERTEXSHADERCONSTANT);
        }
    }

    /* TODO: Keep track of previously enabled clipplanes to avoid unnecessary resetting
     * of already set values
     */

    /* If enabling / disabling all
     * TODO: Is this correct? Doesn't D3DRS_CLIPPING disable clipping on the viewport frustrum?
     */
    if (state->render_states[WINED3D_RS_CLIPPING])
    {
        enable = state->render_states[WINED3D_RS_CLIPPLANEENABLE];
        disable = ~state->render_states[WINED3D_RS_CLIPPLANEENABLE];
    }
    else
    {
        disable = 0xffffffff;
        enable  = 0x00;
    }

    if (enable & WINED3DCLIPPLANE0)  { glEnable(GL_CLIP_PLANE0);  checkGLcall("glEnable(clip plane 0)"); }
    if (enable & WINED3DCLIPPLANE1)  { glEnable(GL_CLIP_PLANE1);  checkGLcall("glEnable(clip plane 1)"); }
    if (enable & WINED3DCLIPPLANE2)  { glEnable(GL_CLIP_PLANE2);  checkGLcall("glEnable(clip plane 2)"); }
    if (enable & WINED3DCLIPPLANE3)  { glEnable(GL_CLIP_PLANE3);  checkGLcall("glEnable(clip plane 3)"); }
    if (enable & WINED3DCLIPPLANE4)  { glEnable(GL_CLIP_PLANE4);  checkGLcall("glEnable(clip plane 4)"); }
    if (enable & WINED3DCLIPPLANE5)  { glEnable(GL_CLIP_PLANE5);  checkGLcall("glEnable(clip plane 5)"); }

    if (disable & WINED3DCLIPPLANE0) { glDisable(GL_CLIP_PLANE0); checkGLcall("glDisable(clip plane 0)"); }
    if (disable & WINED3DCLIPPLANE1) { glDisable(GL_CLIP_PLANE1); checkGLcall("glDisable(clip plane 1)"); }
    if (disable & WINED3DCLIPPLANE2) { glDisable(GL_CLIP_PLANE2); checkGLcall("glDisable(clip plane 2)"); }
    if (disable & WINED3DCLIPPLANE3) { glDisable(GL_CLIP_PLANE3); checkGLcall("glDisable(clip plane 3)"); }
    if (disable & WINED3DCLIPPLANE4) { glDisable(GL_CLIP_PLANE4); checkGLcall("glDisable(clip plane 4)"); }
    if (disable & WINED3DCLIPPLANE5) { glDisable(GL_CLIP_PLANE5); checkGLcall("glDisable(clip plane 5)"); }
}

static void state_specularenable(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    /* Originally this used glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL,GL_SEPARATE_SPECULAR_COLOR)
     * and (GL_LIGHT_MODEL_COLOR_CONTROL,GL_SINGLE_COLOR) to swap between enabled/disabled
     * specular color. This is wrong:
     * Separate specular color means the specular colour is maintained separately, whereas
     * single color means it is merged in. However in both cases they are being used to
     * some extent.
     * To disable specular color, set it explicitly to black and turn off GL_COLOR_SUM_EXT
     * NOTE: If not supported don't give FIXMEs the impact is really minimal and very few people are
     * running 1.4 yet!
     *
     *
     * If register combiners are enabled, enabling / disabling GL_COLOR_SUM has no effect.
     * Instead, we need to setup the FinalCombiner properly.
     *
     * The default setup for the FinalCombiner is:
     *
     * <variable>       <input>                             <mapping>               <usage>
     * GL_VARIABLE_A_NV GL_FOG,                             GL_UNSIGNED_IDENTITY_NV GL_ALPHA
     * GL_VARIABLE_B_NV GL_SPARE0_PLUS_SECONDARY_COLOR_NV   GL_UNSIGNED_IDENTITY_NV GL_RGB
     * GL_VARIABLE_C_NV GL_FOG                              GL_UNSIGNED_IDENTITY_NV GL_RGB
     * GL_VARIABLE_D_NV GL_ZERO                             GL_UNSIGNED_IDENTITY_NV GL_RGB
     * GL_VARIABLE_E_NV GL_ZERO                             GL_UNSIGNED_IDENTITY_NV GL_RGB
     * GL_VARIABLE_F_NV GL_ZERO                             GL_UNSIGNED_IDENTITY_NV GL_RGB
     * GL_VARIABLE_G_NV GL_SPARE0_NV                        GL_UNSIGNED_IDENTITY_NV GL_ALPHA
     *
     * That's pretty much fine as it is, except for variable B, which needs to take
     * either GL_SPARE0_PLUS_SECONDARY_COLOR_NV or GL_SPARE0_NV, depending on
     * whether WINED3D_RS_SPECULARENABLE is enabled or not.
     */

    TRACE("Setting specular enable state and materials\n");
    if (state->render_states[WINED3D_RS_SPECULARENABLE])
    {
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, (float *)&state->material.specular);
        checkGLcall("glMaterialfv");

        if (state->material.power > gl_info->limits.shininess)
        {
            /* glMaterialf man page says that the material says that GL_SHININESS must be between 0.0
             * and 128.0, although in d3d neither -1 nor 129 produce an error. GL_NV_max_light_exponent
             * allows bigger values. If the extension is supported, gl_info->limits.shininess contains the
             * value reported by the extension, otherwise 128. For values > gl_info->limits.shininess clamp
             * them, it should be safe to do so without major visual distortions.
             */
            WARN("Material power = %.8e, limit %.8e\n", state->material.power, gl_info->limits.shininess);
            glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, gl_info->limits.shininess);
        }
        else
        {
            glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, state->material.power);
        }
        checkGLcall("glMaterialf(GL_SHININESS)");

        if (gl_info->supported[EXT_SECONDARY_COLOR])
        {
            glEnable(GL_COLOR_SUM_EXT);
        }
        else
        {
            TRACE("Specular colors cannot be enabled in this version of opengl\n");
        }
        checkGLcall("glEnable(GL_COLOR_SUM)");

        if (gl_info->supported[NV_REGISTER_COMBINERS])
        {
            GL_EXTCALL(glFinalCombinerInputNV(GL_VARIABLE_B_NV, GL_SPARE0_PLUS_SECONDARY_COLOR_NV, GL_UNSIGNED_IDENTITY_NV, GL_RGB));
            checkGLcall("glFinalCombinerInputNV()");
        }
    } else {
        static const GLfloat black[] = {0.0f, 0.0f, 0.0f, 0.0f};

        /* for the case of enabled lighting: */
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, &black[0]);
        checkGLcall("glMaterialfv");

        /* for the case of disabled lighting: */
        if (gl_info->supported[EXT_SECONDARY_COLOR])
        {
            glDisable(GL_COLOR_SUM_EXT);
        }
        else
        {
            TRACE("Specular colors cannot be disabled in this version of opengl\n");
        }
        checkGLcall("glDisable(GL_COLOR_SUM)");

        if (gl_info->supported[NV_REGISTER_COMBINERS])
        {
            GL_EXTCALL(glFinalCombinerInputNV(GL_VARIABLE_B_NV, GL_SPARE0_NV, GL_UNSIGNED_IDENTITY_NV, GL_RGB));
            checkGLcall("glFinalCombinerInputNV()");
        }
    }

    TRACE("diffuse {%.8e, %.8e, %.8e, %.8e}\n",
            state->material.diffuse.r, state->material.diffuse.g,
            state->material.diffuse.b, state->material.diffuse.a);
    TRACE("ambient {%.8e, %.8e, %.8e, %.8e}\n",
            state->material.ambient.r, state->material.ambient.g,
            state->material.ambient.b, state->material.ambient.a);
    TRACE("specular {%.8e, %.8e, %.8e, %.8e}\n",
            state->material.specular.r, state->material.specular.g,
            state->material.specular.b, state->material.specular.a);
    TRACE("emissive {%.8e, %.8e, %.8e, %.8e}\n",
            state->material.emissive.r, state->material.emissive.g,
            state->material.emissive.b, state->material.emissive.a);

    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, (float *)&state->material.ambient);
    checkGLcall("glMaterialfv(GL_AMBIENT)");
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, (float *)&state->material.diffuse);
    checkGLcall("glMaterialfv(GL_DIFFUSE)");
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, (float *)&state->material.emissive);
    checkGLcall("glMaterialfv(GL_EMISSION)");
}

static void state_texfactor(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    unsigned int i;

    /* Note the texture color applies to all textures whereas
     * GL_TEXTURE_ENV_COLOR applies to active only. */
    float col[4];
    D3DCOLORTOGLFLOAT4(state->render_states[WINED3D_RS_TEXTUREFACTOR], col);

    /* And now the default texture color as well */
    for (i = 0; i < gl_info->limits.texture_stages; ++i)
    {
        /* Note the WINED3D_RS value applies to all textures, but GL has one
         * per texture, so apply it now ready to be used! */
        context_active_texture(context, gl_info, i);

        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, &col[0]);
        checkGLcall("glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);");
    }
}

static void renderstate_stencil_twosided(struct wined3d_context *context, GLint face,
        GLint func, GLint ref, GLuint mask, GLint stencilFail, GLint depthFail, GLint stencilPass)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;

    glEnable(GL_STENCIL_TEST_TWO_SIDE_EXT);
    checkGLcall("glEnable(GL_STENCIL_TEST_TWO_SIDE_EXT)");
    GL_EXTCALL(glActiveStencilFaceEXT(face));
    checkGLcall("glActiveStencilFaceEXT(...)");
    glStencilFunc(func, ref, mask);
    checkGLcall("glStencilFunc(...)");
    glStencilOp(stencilFail, depthFail, stencilPass);
    checkGLcall("glStencilOp(...)");
}

static void state_stencil(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    DWORD onesided_enable = FALSE;
    DWORD twosided_enable = FALSE;
    GLint func = GL_ALWAYS;
    GLint func_ccw = GL_ALWAYS;
    GLint ref = 0;
    GLuint mask = 0;
    GLint stencilFail = GL_KEEP;
    GLint depthFail = GL_KEEP;
    GLint stencilPass = GL_KEEP;
    GLint stencilFail_ccw = GL_KEEP;
    GLint depthFail_ccw = GL_KEEP;
    GLint stencilPass_ccw = GL_KEEP;

    /* No stencil test without a stencil buffer. */
    if (!state->fb->depth_stencil)
    {
        glDisable(GL_STENCIL_TEST);
        checkGLcall("glDisable GL_STENCIL_TEST");
        return;
    }

    onesided_enable = state->render_states[WINED3D_RS_STENCILENABLE];
    twosided_enable = state->render_states[WINED3D_RS_TWOSIDEDSTENCILMODE];
    if (!(func = gl_compare_func(state->render_states[WINED3D_RS_STENCILFUNC])))
        func = GL_ALWAYS;
    if (!(func_ccw = gl_compare_func(state->render_states[WINED3D_RS_CCW_STENCILFUNC])))
        func_ccw = GL_ALWAYS;
    ref = state->render_states[WINED3D_RS_STENCILREF];
    mask = state->render_states[WINED3D_RS_STENCILMASK];
    stencilFail = StencilOp(state->render_states[WINED3D_RS_STENCILFAIL]);
    depthFail = StencilOp(state->render_states[WINED3D_RS_STENCILZFAIL]);
    stencilPass = StencilOp(state->render_states[WINED3D_RS_STENCILPASS]);
    stencilFail_ccw = StencilOp(state->render_states[WINED3D_RS_CCW_STENCILFAIL]);
    depthFail_ccw = StencilOp(state->render_states[WINED3D_RS_CCW_STENCILZFAIL]);
    stencilPass_ccw = StencilOp(state->render_states[WINED3D_RS_CCW_STENCILPASS]);

    TRACE("(onesided %d, twosided %d, ref %x, mask %x, "
          "GL_FRONT: func: %x, fail %x, zfail %x, zpass %x "
          "GL_BACK: func: %x, fail %x, zfail %x, zpass %x )\n",
    onesided_enable, twosided_enable, ref, mask,
    func, stencilFail, depthFail, stencilPass,
    func_ccw, stencilFail_ccw, depthFail_ccw, stencilPass_ccw);

    if (twosided_enable && onesided_enable) {
        glEnable(GL_STENCIL_TEST);
        checkGLcall("glEnable GL_STENCIL_TEST");

        if (gl_info->supported[EXT_STENCIL_TWO_SIDE])
        {
            /* Apply back first, then front. This function calls glActiveStencilFaceEXT,
             * which has an effect on the code below too. If we apply the front face
             * afterwards, we are sure that the active stencil face is set to front,
             * and other stencil functions which do not use two sided stencil do not have
             * to set it back
             */
            renderstate_stencil_twosided(context, GL_BACK,
                    func_ccw, ref, mask, stencilFail_ccw, depthFail_ccw, stencilPass_ccw);
            renderstate_stencil_twosided(context, GL_FRONT,
                    func, ref, mask, stencilFail, depthFail, stencilPass);
        }
        else if (gl_info->supported[ATI_SEPARATE_STENCIL])
        {
            GL_EXTCALL(glStencilFuncSeparateATI(func, func_ccw, ref, mask));
            checkGLcall("glStencilFuncSeparateATI(...)");
            GL_EXTCALL(glStencilOpSeparateATI(GL_FRONT, stencilFail, depthFail, stencilPass));
            checkGLcall("glStencilOpSeparateATI(GL_FRONT, ...)");
            GL_EXTCALL(glStencilOpSeparateATI(GL_BACK, stencilFail_ccw, depthFail_ccw, stencilPass_ccw));
            checkGLcall("glStencilOpSeparateATI(GL_BACK, ...)");
        } else {
            ERR("Separate (two sided) stencil not supported on this version of opengl. Caps weren't honored?\n");
        }
    }
    else if(onesided_enable)
    {
        if (gl_info->supported[EXT_STENCIL_TWO_SIDE])
        {
            glDisable(GL_STENCIL_TEST_TWO_SIDE_EXT);
            checkGLcall("glDisable(GL_STENCIL_TEST_TWO_SIDE_EXT)");
        }

        /* This code disables the ATI extension as well, since the standard stencil functions are equal
         * to calling the ATI functions with GL_FRONT_AND_BACK as face parameter
         */
        glEnable(GL_STENCIL_TEST);
        checkGLcall("glEnable GL_STENCIL_TEST");
        glStencilFunc(func, ref, mask);
        checkGLcall("glStencilFunc(...)");
        glStencilOp(stencilFail, depthFail, stencilPass);
        checkGLcall("glStencilOp(...)");
    } else {
        glDisable(GL_STENCIL_TEST);
        checkGLcall("glDisable GL_STENCIL_TEST");
    }
}

static void state_stencilwrite2s(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD mask = state->fb->depth_stencil ? state->render_states[WINED3D_RS_STENCILWRITEMASK] : 0;
    const struct wined3d_gl_info *gl_info = context->gl_info;

    GL_EXTCALL(glActiveStencilFaceEXT(GL_BACK));
    checkGLcall("glActiveStencilFaceEXT(GL_BACK)");
    glStencilMask(mask);
    checkGLcall("glStencilMask");
    GL_EXTCALL(glActiveStencilFaceEXT(GL_FRONT));
    checkGLcall("glActiveStencilFaceEXT(GL_FRONT)");
    glStencilMask(mask);
}

static void state_stencilwrite(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD mask = state->fb->depth_stencil ? state->render_states[WINED3D_RS_STENCILWRITEMASK] : 0;

    glStencilMask(mask);
    checkGLcall("glStencilMask");
}

static void state_fog_vertexpart(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{

    const struct wined3d_gl_info *gl_info = context->gl_info;
    TRACE("context %p, state %p, state_id %#x.\n", context, state, state_id);

    if (!state->render_states[WINED3D_RS_FOGENABLE])
        return;

    /* Table fog on: Never use fog coords, and use per-fragment fog */
    if (state->render_states[WINED3D_RS_FOGTABLEMODE] != WINED3D_FOG_NONE)
    {
        glHint(GL_FOG_HINT, GL_NICEST);
        if(context->fog_coord) {
            glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FRAGMENT_DEPTH_EXT);
            checkGLcall("glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FRAGMENT_DEPTH_EXT)");
            context->fog_coord = FALSE;
        }

        /* Range fog is only used with per-vertex fog in d3d */
        if (gl_info->supported[NV_FOG_DISTANCE])
        {
            glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_PLANE_ABSOLUTE_NV);
            checkGLcall("glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_PLANE_ABSOLUTE_NV)");
        }
        return;
    }

    /* Otherwise use per-vertex fog in any case */
    glHint(GL_FOG_HINT, GL_FASTEST);

    if (state->render_states[WINED3D_RS_FOGVERTEXMODE] == WINED3D_FOG_NONE || context->last_was_rhw)
    {
        /* No fog at all, or transformed vertices: Use fog coord */
        if(!context->fog_coord) {
            glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FOG_COORDINATE_EXT);
            checkGLcall("glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FOG_COORDINATE_EXT)");
            context->fog_coord = TRUE;
        }
    }
    else
    {
        /* Otherwise, use the fragment depth */
        if(context->fog_coord) {
            glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FRAGMENT_DEPTH_EXT);
            checkGLcall("glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FRAGMENT_DEPTH_EXT)");
            context->fog_coord = FALSE;
        }

        if (state->render_states[WINED3D_RS_RANGEFOGENABLE])
        {
            if (gl_info->supported[NV_FOG_DISTANCE])
            {
                glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV);
                checkGLcall("glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV)");
            }
            else
            {
                WARN("Range fog enabled, but not supported by this GL implementation.\n");
            }
        }
        else if (gl_info->supported[NV_FOG_DISTANCE])
        {
            glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_PLANE_ABSOLUTE_NV);
            checkGLcall("glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_PLANE_ABSOLUTE_NV)");
        }
    }
}

void state_fogstartend(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    float fogstart, fogend;
    union {
        DWORD d;
        float f;
    } tmpvalue;

    switch(context->fog_source) {
        case FOGSOURCE_VS:
            fogstart = 1.0f;
            fogend = 0.0f;
            break;

        case FOGSOURCE_COORD:
            fogstart = 255.0f;
            fogend = 0.0f;
            break;

        case FOGSOURCE_FFP:
            tmpvalue.d = state->render_states[WINED3D_RS_FOGSTART];
            fogstart = tmpvalue.f;
            tmpvalue.d = state->render_states[WINED3D_RS_FOGEND];
            fogend = tmpvalue.f;
            /* In GL, fogstart == fogend disables fog, in D3D everything's fogged.*/
            if(fogstart == fogend) {
                fogstart = -1.0f / 0.0f;
                fogend = 0.0f;
            }
            break;

        default:
            /* This should not happen.context->fog_source is set in wined3d, not the app.
             * Still this is needed to make the compiler happy
             */
            ERR("Unexpected fog coordinate source\n");
            fogstart = 0.0f;
            fogend = 0.0f;
    }

    glFogf(GL_FOG_START, fogstart);
    checkGLcall("glFogf(GL_FOG_START, fogstart)");
    TRACE("Fog Start == %f\n", fogstart);

    glFogf(GL_FOG_END, fogend);
    checkGLcall("glFogf(GL_FOG_END, fogend)");
    TRACE("Fog End == %f\n", fogend);
}

void state_fog_fragpart(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    enum fogsource new_source;

    TRACE("context %p, state %p, state_id %#x.\n", context, state, state_id);

    if (!state->render_states[WINED3D_RS_FOGENABLE])
    {
        /* No fog? Disable it, and we're done :-) */
        glDisableWINE(GL_FOG);
        checkGLcall("glDisable GL_FOG");
        return;
    }

    /* Fog Rules:
     *
     * With fixed function vertex processing, Direct3D knows 2 different fog input sources.
     * It can use the Z value of the vertex, or the alpha component of the specular color.
     * This depends on the fog vertex, fog table and the vertex declaration. If the Z value
     * is used, fogstart, fogend and the equation type are used, otherwise linear fog with
     * start = 255, end = 0 is used. Obviously the msdn is not very clear on that.
     *
     * FOGTABLEMODE != NONE:
     *  The Z value is used, with the equation specified, no matter what vertex type.
     *
     * FOGTABLEMODE == NONE, FOGVERTEXMODE != NONE, untransformed:
     *  Per vertex fog is calculated using the specified fog equation and the parameters
     *
     * FOGTABLEMODE == NONE, FOGVERTEXMODE != NONE, transformed, OR
     * FOGTABLEMODE == NONE, FOGVERTEXMODE == NONE, untransformed:
     *  Linear fog with start = 255.0, end = 0.0, input comes from the specular color
     *
     *
     * Rules for vertex fog with shaders:
     *
     * When mixing fixed function functionality with the programmable pipeline, D3D expects
     * the fog computation to happen during transformation while openGL expects it to happen
     * during rasterization. Also, prior to pixel shader 3.0 D3D handles fog blending after
     * the pixel shader while openGL always expects the pixel shader to handle the blending.
     * To solve this problem, WineD3D does:
     * 1) implement a linear fog equation and fog blending at the end of every pre 3.0 pixel
     * shader,
     * and 2) disables the fog computation (in either the fixed function or programmable
     * rasterizer) if using a vertex program.
     *
     * D3D shaders can provide an explicit fog coordinate. This fog coordinate is used with
     * D3DRS_FOGTABLEMODE==D3DFOG_NONE. The FOGVERTEXMODE is ignored, d3d always uses linear
     * fog with start=1.0 and end=0.0 in this case. This is similar to fog coordinates in
     * the specular color, a vertex shader counts as pretransformed geometry in this case.
     * There are some GL differences between specular fog coords and vertex shaders though.
     *
     * With table fog the vertex shader fog coordinate is ignored.
     *
     * If a fogtablemode and a fogvertexmode are specified, table fog is applied (with or
     * without shaders).
     */

    /* DX 7 sdk: "If both render states(vertex and table fog) are set to valid modes,
     * the system will apply only pixel(=table) fog effects."
     */
    if (state->render_states[WINED3D_RS_FOGTABLEMODE] == WINED3D_FOG_NONE)
    {
        if (use_vs(state))
        {
            glFogi(GL_FOG_MODE, GL_LINEAR);
            checkGLcall("glFogi(GL_FOG_MODE, GL_LINEAR)");
            new_source = FOGSOURCE_VS;
        }
        else
        {
            switch (state->render_states[WINED3D_RS_FOGVERTEXMODE])
            {
                /* If processed vertices are used, fall through to the NONE case */
                case WINED3D_FOG_EXP:
                    if (!context->last_was_rhw)
                    {
                        glFogi(GL_FOG_MODE, GL_EXP);
                        checkGLcall("glFogi(GL_FOG_MODE, GL_EXP)");
                        new_source = FOGSOURCE_FFP;
                        break;
                    }
                    /* drop through */

                case WINED3D_FOG_EXP2:
                    if (!context->last_was_rhw)
                    {
                        glFogi(GL_FOG_MODE, GL_EXP2);
                        checkGLcall("glFogi(GL_FOG_MODE, GL_EXP2)");
                        new_source = FOGSOURCE_FFP;
                        break;
                    }
                    /* drop through */

                case WINED3D_FOG_LINEAR:
                    if (!context->last_was_rhw)
                    {
                        glFogi(GL_FOG_MODE, GL_LINEAR);
                        checkGLcall("glFogi(GL_FOG_MODE, GL_LINEAR)");
                        new_source = FOGSOURCE_FFP;
                        break;
                    }
                    /* drop through */

                case WINED3D_FOG_NONE:
                    /* Both are none? According to msdn the alpha channel of the specular
                     * color contains a fog factor. Set it in drawStridedSlow.
                     * Same happens with Vertexfog on transformed vertices
                     */
                    new_source = FOGSOURCE_COORD;
                    glFogi(GL_FOG_MODE, GL_LINEAR);
                    checkGLcall("glFogi(GL_FOG_MODE, GL_LINEAR)");
                    break;

                default:
                    FIXME("Unexpected WINED3D_RS_FOGVERTEXMODE %#x.\n",
                            state->render_states[WINED3D_RS_FOGVERTEXMODE]);
                    new_source = FOGSOURCE_FFP; /* Make the compiler happy */
            }
        }
    } else {
        new_source = FOGSOURCE_FFP;

        switch (state->render_states[WINED3D_RS_FOGTABLEMODE])
        {
            case WINED3D_FOG_EXP:
                glFogi(GL_FOG_MODE, GL_EXP);
                checkGLcall("glFogi(GL_FOG_MODE, GL_EXP)");
                break;

            case WINED3D_FOG_EXP2:
                glFogi(GL_FOG_MODE, GL_EXP2);
                checkGLcall("glFogi(GL_FOG_MODE, GL_EXP2)");
                break;

            case WINED3D_FOG_LINEAR:
                glFogi(GL_FOG_MODE, GL_LINEAR);
                checkGLcall("glFogi(GL_FOG_MODE, GL_LINEAR)");
                break;

            case WINED3D_FOG_NONE:   /* Won't happen */
            default:
                FIXME("Unexpected WINED3D_RS_FOGTABLEMODE %#x.\n",
                        state->render_states[WINED3D_RS_FOGTABLEMODE]);
        }
    }

    glEnableWINE(GL_FOG);
    checkGLcall("glEnable GL_FOG");
    if (new_source != context->fog_source)
    {
        context->fog_source = new_source;
        state_fogstartend(context, state, STATE_RENDER(WINED3D_RS_FOGSTART));
    }
}

void state_fogcolor(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    float col[4];

    D3DCOLORTOGLFLOAT4(state->render_states[WINED3D_RS_FOGCOLOR], col);
    glFogfv(GL_FOG_COLOR, &col[0]);
    checkGLcall("glFog GL_FOG_COLOR");
}

void state_fogdensity(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    union {
        DWORD d;
        float f;
    } tmpvalue;

    tmpvalue.d = state->render_states[WINED3D_RS_FOGDENSITY];
    glFogfv(GL_FOG_DENSITY, &tmpvalue.f);
    checkGLcall("glFogf(GL_FOG_DENSITY, (float) Value)");
}

static void state_colormat(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_device *device = context->swapchain->device;
    GLenum Parm = 0;

    /* Depends on the decoded vertex declaration to read the existence of diffuse data.
     * The vertex declaration will call this function if the fixed function pipeline is used.
     */

    if(isStateDirty(context, STATE_VDECL)) {
        return;
    }

    context->num_untracked_materials = 0;
    if ((device->strided_streams.use_map & (1 << WINED3D_FFP_DIFFUSE))
            && state->render_states[WINED3D_RS_COLORVERTEX])
    {
        TRACE("diff %d, amb %d, emis %d, spec %d\n",
                state->render_states[WINED3D_RS_DIFFUSEMATERIALSOURCE],
                state->render_states[WINED3D_RS_AMBIENTMATERIALSOURCE],
                state->render_states[WINED3D_RS_EMISSIVEMATERIALSOURCE],
                state->render_states[WINED3D_RS_SPECULARMATERIALSOURCE]);

        if (state->render_states[WINED3D_RS_DIFFUSEMATERIALSOURCE] == WINED3DMCS_COLOR1)
        {
            if (state->render_states[WINED3D_RS_AMBIENTMATERIALSOURCE] == WINED3DMCS_COLOR1)
                Parm = GL_AMBIENT_AND_DIFFUSE;
            else
                Parm = GL_DIFFUSE;
            if (state->render_states[WINED3D_RS_EMISSIVEMATERIALSOURCE] == WINED3DMCS_COLOR1)
            {
                context->untracked_materials[context->num_untracked_materials] = GL_EMISSION;
                context->num_untracked_materials++;
            }
            if (state->render_states[WINED3D_RS_SPECULARMATERIALSOURCE] == WINED3DMCS_COLOR1)
            {
                context->untracked_materials[context->num_untracked_materials] = GL_SPECULAR;
                context->num_untracked_materials++;
            }
        }
        else if (state->render_states[WINED3D_RS_AMBIENTMATERIALSOURCE] == WINED3DMCS_COLOR1)
        {
            Parm = GL_AMBIENT;
            if (state->render_states[WINED3D_RS_EMISSIVEMATERIALSOURCE] == WINED3DMCS_COLOR1)
            {
                context->untracked_materials[context->num_untracked_materials] = GL_EMISSION;
                context->num_untracked_materials++;
            }
            if (state->render_states[WINED3D_RS_SPECULARMATERIALSOURCE] == WINED3DMCS_COLOR1)
            {
                context->untracked_materials[context->num_untracked_materials] = GL_SPECULAR;
                context->num_untracked_materials++;
            }
        }
        else if (state->render_states[WINED3D_RS_EMISSIVEMATERIALSOURCE] == WINED3DMCS_COLOR1)
        {
            Parm = GL_EMISSION;
            if (state->render_states[WINED3D_RS_SPECULARMATERIALSOURCE] == WINED3DMCS_COLOR1)
            {
                context->untracked_materials[context->num_untracked_materials] = GL_SPECULAR;
                context->num_untracked_materials++;
            }
        }
        else if (state->render_states[WINED3D_RS_SPECULARMATERIALSOURCE] == WINED3DMCS_COLOR1)
        {
            Parm = GL_SPECULAR;
        }
    }

    /* Nothing changed, return. */
    if (Parm == context->tracking_parm) return;

    if(!Parm) {
        glDisable(GL_COLOR_MATERIAL);
        checkGLcall("glDisable GL_COLOR_MATERIAL");
    } else {
        glColorMaterial(GL_FRONT_AND_BACK, Parm);
        checkGLcall("glColorMaterial(GL_FRONT_AND_BACK, Parm)");
        glEnable(GL_COLOR_MATERIAL);
        checkGLcall("glEnable(GL_COLOR_MATERIAL)");
    }

    /* Apparently calls to glMaterialfv are ignored for properties we're
     * tracking with glColorMaterial, so apply those here. */
    switch (context->tracking_parm) {
        case GL_AMBIENT_AND_DIFFUSE:
            glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, (float *)&state->material.ambient);
            glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, (float *)&state->material.diffuse);
            checkGLcall("glMaterialfv");
            break;

        case GL_DIFFUSE:
            glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, (float *)&state->material.diffuse);
            checkGLcall("glMaterialfv");
            break;

        case GL_AMBIENT:
            glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, (float *)&state->material.ambient);
            checkGLcall("glMaterialfv");
            break;

        case GL_EMISSION:
            glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, (float *)&state->material.emissive);
            checkGLcall("glMaterialfv");
            break;

        case GL_SPECULAR:
            /* Only change material color if specular is enabled, otherwise it is set to black */
            if (state->render_states[WINED3D_RS_SPECULARENABLE])
            {
                glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, (float *)&state->material.specular);
                checkGLcall("glMaterialfv");
            }
            else
            {
                static const GLfloat black[] = {0.0f, 0.0f, 0.0f, 0.0f};
                glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, &black[0]);
                checkGLcall("glMaterialfv");
            }
            break;
    }

    context->tracking_parm = Parm;
}

static void state_linepattern(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    union
    {
        DWORD d;
        struct wined3d_line_pattern lp;
    } tmppattern;
    tmppattern.d = state->render_states[WINED3D_RS_LINEPATTERN];

    TRACE("Line pattern: repeat %d bits %x.\n", tmppattern.lp.repeat_factor, tmppattern.lp.line_pattern);

    if (tmppattern.lp.repeat_factor)
    {
        glLineStipple(tmppattern.lp.repeat_factor, tmppattern.lp.line_pattern);
        checkGLcall("glLineStipple(repeat, linepattern)");
        glEnable(GL_LINE_STIPPLE);
        checkGLcall("glEnable(GL_LINE_STIPPLE);");
    }
    else
    {
        glDisable(GL_LINE_STIPPLE);
        checkGLcall("glDisable(GL_LINE_STIPPLE);");
    }
}

static void state_normalize(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (isStateDirty(context, STATE_VDECL))
        return;

    /* Without vertex normals, we set the current normal to 0/0/0 to remove the diffuse factor
     * from the opengl lighting equation, as d3d does. Normalization of 0/0/0 can lead to a division
     * by zero and is not properly defined in opengl, so avoid it
     */
    if (state->render_states[WINED3D_RS_NORMALIZENORMALS]
            && (context->swapchain->device->strided_streams.use_map & (1 << WINED3D_FFP_NORMAL)))
    {
        glEnable(GL_NORMALIZE);
        checkGLcall("glEnable(GL_NORMALIZE);");
    }
    else
    {
        glDisable(GL_NORMALIZE);
        checkGLcall("glDisable(GL_NORMALIZE);");
    }
}

static void state_psizemin_w(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    union {
        DWORD d;
        float f;
    } tmpvalue;

    tmpvalue.d = state->render_states[WINED3D_RS_POINTSIZE_MIN];
    if (tmpvalue.f != 1.0f)
    {
        FIXME("WINED3D_RS_POINTSIZE_MIN not supported on this opengl, value is %f\n", tmpvalue.f);
    }
    tmpvalue.d = state->render_states[WINED3D_RS_POINTSIZE_MAX];
    if (tmpvalue.f != 64.0f)
    {
        FIXME("WINED3D_RS_POINTSIZE_MAX not supported on this opengl, value is %f\n", tmpvalue.f);
    }

}

static void state_psizemin_ext(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    union
    {
        DWORD d;
        float f;
    } min, max;

    min.d = state->render_states[WINED3D_RS_POINTSIZE_MIN];
    max.d = state->render_states[WINED3D_RS_POINTSIZE_MAX];

    /* Max point size trumps min point size */
    if(min.f > max.f) {
        min.f = max.f;
    }

    GL_EXTCALL(glPointParameterfEXT)(GL_POINT_SIZE_MIN_EXT, min.f);
    checkGLcall("glPointParameterfEXT(...)");
    GL_EXTCALL(glPointParameterfEXT)(GL_POINT_SIZE_MAX_EXT, max.f);
    checkGLcall("glPointParameterfEXT(...)");
}

static void state_psizemin_arb(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    union
    {
        DWORD d;
        float f;
    } min, max;

    min.d = state->render_states[WINED3D_RS_POINTSIZE_MIN];
    max.d = state->render_states[WINED3D_RS_POINTSIZE_MAX];

    /* Max point size trumps min point size */
    if(min.f > max.f) {
        min.f = max.f;
    }

    GL_EXTCALL(glPointParameterfARB)(GL_POINT_SIZE_MIN_ARB, min.f);
    checkGLcall("glPointParameterfARB(...)");
    GL_EXTCALL(glPointParameterfARB)(GL_POINT_SIZE_MAX_ARB, max.f);
    checkGLcall("glPointParameterfARB(...)");
}

static void state_pscale(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    /* TODO: Group this with the viewport */
    /*
     * POINTSCALEENABLE controls how point size value is treated. If set to
     * true, the point size is scaled with respect to height of viewport.
     * When set to false point size is in pixels.
     */

    /* Default values */
    GLfloat att[3] = {1.0f, 0.0f, 0.0f};
    union {
        DWORD d;
        float f;
    } pointSize, A, B, C;

    pointSize.d = state->render_states[WINED3D_RS_POINTSIZE];
    A.d = state->render_states[WINED3D_RS_POINTSCALE_A];
    B.d = state->render_states[WINED3D_RS_POINTSCALE_B];
    C.d = state->render_states[WINED3D_RS_POINTSCALE_C];

    if (state->render_states[WINED3D_RS_POINTSCALEENABLE])
    {
        DWORD h = state->viewport.height;
        GLfloat scaleFactor;

        if (pointSize.f < gl_info->limits.pointsize_min)
        {
            /* Minimum valid point size for OpenGL is driver specific. For Direct3D it is
             * 0.0f. This means that OpenGL will clamp really small point sizes to the
             * driver minimum. To correct for this we need to multiply by the scale factor when sizes
             * are less than 1.0f. scale_factor =  1.0f / point_size.
             */
            scaleFactor = pointSize.f / gl_info->limits.pointsize_min;
            /* Clamp the point size, don't rely on the driver to do it. MacOS says min point size
             * is 1.0, but then accepts points below that and draws too small points
             */
            pointSize.f = gl_info->limits.pointsize_min;
        }
        else if(pointSize.f > gl_info->limits.pointsize_max)
        {
            /* gl already scales the input to glPointSize,
             * d3d scales the result after the point size scale.
             * If the point size is bigger than the max size, use the
             * scaling to scale it bigger, and set the gl point size to max
             */
            scaleFactor = pointSize.f / gl_info->limits.pointsize_max;
            TRACE("scale: %f\n", scaleFactor);
            pointSize.f = gl_info->limits.pointsize_max;
        } else {
            scaleFactor = 1.0f;
        }
        scaleFactor = powf(h * scaleFactor, 2);

        att[0] = A.f / scaleFactor;
        att[1] = B.f / scaleFactor;
        att[2] = C.f / scaleFactor;
    }

    if (gl_info->supported[ARB_POINT_PARAMETERS])
    {
        GL_EXTCALL(glPointParameterfvARB)(GL_POINT_DISTANCE_ATTENUATION_ARB, att);
        checkGLcall("glPointParameterfvARB(GL_DISTANCE_ATTENUATION_ARB, ...)");
    }
    else if (gl_info->supported[EXT_POINT_PARAMETERS])
    {
        GL_EXTCALL(glPointParameterfvEXT)(GL_DISTANCE_ATTENUATION_EXT, att);
        checkGLcall("glPointParameterfvEXT(GL_DISTANCE_ATTENUATION_EXT, ...)");
    }
    else if (state->render_states[WINED3D_RS_POINTSCALEENABLE])
    {
        WARN("POINT_PARAMETERS not supported in this version of opengl\n");
    }

    glPointSize(pointSize.f);
    checkGLcall("glPointSize(...);");
}

static void state_debug_monitor(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    WARN("token: %#x.\n", state->render_states[WINED3D_RS_DEBUGMONITORTOKEN]);
}

static void state_colorwrite(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD mask0 = state->render_states[WINED3D_RS_COLORWRITEENABLE];
    DWORD mask1 = state->render_states[WINED3D_RS_COLORWRITEENABLE1];
    DWORD mask2 = state->render_states[WINED3D_RS_COLORWRITEENABLE2];
    DWORD mask3 = state->render_states[WINED3D_RS_COLORWRITEENABLE3];

    TRACE("Color mask: r(%d) g(%d) b(%d) a(%d)\n",
            mask0 & WINED3DCOLORWRITEENABLE_RED ? 1 : 0,
            mask0 & WINED3DCOLORWRITEENABLE_GREEN ? 1 : 0,
            mask0 & WINED3DCOLORWRITEENABLE_BLUE ? 1 : 0,
            mask0 & WINED3DCOLORWRITEENABLE_ALPHA ? 1 : 0);
    glColorMask(mask0 & WINED3DCOLORWRITEENABLE_RED ? GL_TRUE : GL_FALSE,
            mask0 & WINED3DCOLORWRITEENABLE_GREEN ? GL_TRUE : GL_FALSE,
            mask0 & WINED3DCOLORWRITEENABLE_BLUE ? GL_TRUE : GL_FALSE,
            mask0 & WINED3DCOLORWRITEENABLE_ALPHA ? GL_TRUE : GL_FALSE);
    checkGLcall("glColorMask(...)");

    if (!((mask1 == mask0 && mask2 == mask0 && mask3 == mask0)
        || (mask1 == 0xf && mask2 == 0xf && mask3 == 0xf)))
    {
        FIXME("WINED3D_RS_COLORWRITEENABLE/1/2/3, %#x/%#x/%#x/%#x not yet implemented.\n",
            mask0, mask1, mask2, mask3);
        FIXME("Missing of cap D3DPMISCCAPS_INDEPENDENTWRITEMASKS wasn't honored?\n");
    }
}

static void set_color_mask(const struct wined3d_gl_info *gl_info, UINT index, DWORD mask)
{
    GL_EXTCALL(glColorMaskIndexedEXT(index,
            mask & WINED3DCOLORWRITEENABLE_RED ? GL_TRUE : GL_FALSE,
            mask & WINED3DCOLORWRITEENABLE_GREEN ? GL_TRUE : GL_FALSE,
            mask & WINED3DCOLORWRITEENABLE_BLUE ? GL_TRUE : GL_FALSE,
            mask & WINED3DCOLORWRITEENABLE_ALPHA ? GL_TRUE : GL_FALSE));
}

static void state_colorwrite0(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    set_color_mask(context->gl_info, 0, state->render_states[WINED3D_RS_COLORWRITEENABLE]);
}

static void state_colorwrite1(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    set_color_mask(context->gl_info, 1, state->render_states[WINED3D_RS_COLORWRITEENABLE1]);
}

static void state_colorwrite2(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    set_color_mask(context->gl_info, 2, state->render_states[WINED3D_RS_COLORWRITEENABLE2]);
}

static void state_colorwrite3(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    set_color_mask(context->gl_info, 3, state->render_states[WINED3D_RS_COLORWRITEENABLE3]);
}

static void state_localviewer(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_LOCALVIEWER])
    {
        glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 1);
        checkGLcall("glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 1)");
    } else {
        glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 0);
        checkGLcall("glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 0)");
    }
}

static void state_lastpixel(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_LASTPIXEL])
    {
        TRACE("Last Pixel Drawing Enabled\n");
    }
    else
    {
        static BOOL warned;
        if (!warned) {
            FIXME("Last Pixel Drawing Disabled, not handled yet\n");
            warned = TRUE;
        } else {
            TRACE("Last Pixel Drawing Disabled, not handled yet\n");
        }
    }
}

static void state_pointsprite_w(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    static BOOL warned;

    /* TODO: NV_POINT_SPRITE */
    if (!warned && state->render_states[WINED3D_RS_POINTSPRITEENABLE])
    {
        /* A FIXME, not a WARN because point sprites should be software emulated if not supported by HW */
        FIXME("Point sprites not supported\n");
        warned = TRUE;
    }
}

static void state_pointsprite(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_POINTSPRITEENABLE])
    {
        glEnable(GL_POINT_SPRITE_ARB);
        checkGLcall("glEnable(GL_POINT_SPRITE_ARB)");
    } else {
        glDisable(GL_POINT_SPRITE_ARB);
        checkGLcall("glDisable(GL_POINT_SPRITE_ARB)");
    }
}

static void state_wrap(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_WRAP0]
            || state->render_states[WINED3D_RS_WRAP1]
            || state->render_states[WINED3D_RS_WRAP2]
            || state->render_states[WINED3D_RS_WRAP3]
            || state->render_states[WINED3D_RS_WRAP4]
            || state->render_states[WINED3D_RS_WRAP5]
            || state->render_states[WINED3D_RS_WRAP6]
            || state->render_states[WINED3D_RS_WRAP7]
            || state->render_states[WINED3D_RS_WRAP8]
            || state->render_states[WINED3D_RS_WRAP9]
            || state->render_states[WINED3D_RS_WRAP10]
            || state->render_states[WINED3D_RS_WRAP11]
            || state->render_states[WINED3D_RS_WRAP12]
            || state->render_states[WINED3D_RS_WRAP13]
            || state->render_states[WINED3D_RS_WRAP14]
            || state->render_states[WINED3D_RS_WRAP15])
        FIXME("(WINED3D_RS_WRAP0) Texture wrapping not yet supported.\n");
}

static void state_msaa_w(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_MULTISAMPLEANTIALIAS])
        WARN("Multisample antialiasing not supported by GL.\n");
}

static void state_msaa(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_MULTISAMPLEANTIALIAS])
    {
        glEnable(GL_MULTISAMPLE_ARB);
        checkGLcall("glEnable(GL_MULTISAMPLE_ARB)");
    } else {
        glDisable(GL_MULTISAMPLE_ARB);
        checkGLcall("glDisable(GL_MULTISAMPLE_ARB)");
    }
}

static void state_scissor(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_SCISSORTESTENABLE])
    {
        glEnable(GL_SCISSOR_TEST);
        checkGLcall("glEnable(GL_SCISSOR_TEST)");
    } else {
        glDisable(GL_SCISSOR_TEST);
        checkGLcall("glDisable(GL_SCISSOR_TEST)");
    }
}

/* The Direct3D depth bias is specified in normalized depth coordinates. In
 * OpenGL the bias is specified in units of "the smallest value that is
 * guaranteed to produce a resolvable offset for a given implementation". To
 * convert from D3D to GL we need to divide the D3D depth bias by that value.
 * There's no practical way to retrieve that value from a given GL
 * implementation, but the D3D application has essentially the same problem,
 * which makes a guess of the depth buffer format's highest possible value a
 * reasonable guess. Note that SLOPESCALEDEPTHBIAS is a scaling factor for the
 * depth slope, and doesn't need to be scaled. */
static void state_depthbias(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_SLOPESCALEDEPTHBIAS]
            || state->render_states[WINED3D_RS_DEPTHBIAS])
    {
        const struct wined3d_surface *depth = state->fb->depth_stencil;
        float scale;

        union
        {
            DWORD d;
            float f;
        } scale_bias, const_bias;

        scale_bias.d = state->render_states[WINED3D_RS_SLOPESCALEDEPTHBIAS];
        const_bias.d = state->render_states[WINED3D_RS_DEPTHBIAS];

        glEnable(GL_POLYGON_OFFSET_FILL);
        checkGLcall("glEnable(GL_POLYGON_OFFSET_FILL)");

        if (context->swapchain->device->wined3d->flags & WINED3D_LEGACY_DEPTH_BIAS)
        {
            float bias = -(float)const_bias.d;
            glPolygonOffset(bias, bias);
            checkGLcall("glPolygonOffset");
        }
        else
        {
            if (depth)
            {
                const struct wined3d_format *fmt = depth->resource.format;
                scale = powf(2, fmt->depth_size) - 1;
                TRACE("Depth format %s, using depthbias scale of %.8e.\n",
                      debug_d3dformat(fmt->id), scale);
            }
            else
            {
                /* The context manager will reapply this state on a depth stencil change */
                TRACE("No depth stencil, using depthbias scale of 0.0.\n");
                scale = 0.0f;
            }

            glPolygonOffset(scale_bias.f, const_bias.f * scale);
            checkGLcall("glPolygonOffset(...)");
        }
    }
    else
    {
        glDisable(GL_POLYGON_OFFSET_FILL);
        checkGLcall("glDisable(GL_POLYGON_OFFSET_FILL)");
    }
}

static void state_zvisible(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_ZVISIBLE])
        FIXME("WINED3D_RS_ZVISIBLE not implemented.\n");
}

static void state_perspective(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_TEXTUREPERSPECTIVE])
    {
        glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
        checkGLcall("glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST)");
    } else {
        glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
        checkGLcall("glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST)");
    }
}

static void state_stippledalpha(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_STIPPLEDALPHA])
        FIXME("Stippled Alpha not supported yet.\n");
}

static void state_antialias(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_ANTIALIAS])
        FIXME("Antialias not supported yet.\n");
}

static void state_multisampmask(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_MULTISAMPLEMASK] != 0xffffffff)
        FIXME("WINED3D_RS_MULTISAMPLEMASK %#x not yet implemented.\n",
                state->render_states[WINED3D_RS_MULTISAMPLEMASK]);
}

static void state_patchedgestyle(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_PATCHEDGESTYLE] != WINED3DPATCHEDGE_DISCRETE)
        FIXME("WINED3D_RS_PATCHEDGESTYLE %#x not yet implemented.\n",
                state->render_states[WINED3D_RS_PATCHEDGESTYLE]);
}

static void state_patchsegments(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    union {
        DWORD d;
        float f;
    } tmpvalue;
    tmpvalue.f = 1.0f;

    if (state->render_states[WINED3D_RS_PATCHSEGMENTS] != tmpvalue.d)
    {
        static BOOL displayed = FALSE;

        tmpvalue.d = state->render_states[WINED3D_RS_PATCHSEGMENTS];
        if(!displayed)
            FIXME("(WINED3D_RS_PATCHSEGMENTS,%f) not yet implemented\n", tmpvalue.f);

        displayed = TRUE;
    }
}

static void state_positiondegree(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_POSITIONDEGREE] != WINED3D_DEGREE_CUBIC)
        FIXME("WINED3D_RS_POSITIONDEGREE %#x not yet implemented.\n",
                state->render_states[WINED3D_RS_POSITIONDEGREE]);
}

static void state_normaldegree(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_NORMALDEGREE] != WINED3D_DEGREE_LINEAR)
        FIXME("WINED3D_RS_NORMALDEGREE %#x not yet implemented.\n",
                state->render_states[WINED3D_RS_NORMALDEGREE]);
}

static void state_tessellation(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_ENABLEADAPTIVETESSELLATION])
        FIXME("WINED3D_RS_ENABLEADAPTIVETESSELLATION %#x not yet implemented.\n",
                state->render_states[WINED3D_RS_ENABLEADAPTIVETESSELLATION]);
}

static void state_nvdb(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    union {
        DWORD d;
        float f;
    } zmin, zmax;

    const struct wined3d_gl_info *gl_info = context->gl_info;

    if (state->render_states[WINED3D_RS_ADAPTIVETESS_X] == WINED3DFMT_NVDB)
    {
        zmin.d = state->render_states[WINED3D_RS_ADAPTIVETESS_Z];
        zmax.d = state->render_states[WINED3D_RS_ADAPTIVETESS_W];

        /* If zmin is larger than zmax INVALID_VALUE error is generated.
         * In d3d9 test is not performed in this case*/
        if (zmin.f <= zmax.f)
        {
            glEnable(GL_DEPTH_BOUNDS_TEST_EXT);
            checkGLcall("glEnable(GL_DEPTH_BOUNDS_TEST_EXT)");
            GL_EXTCALL(glDepthBoundsEXT(zmin.f, zmax.f));
            checkGLcall("glDepthBoundsEXT(...)");
        }
        else {
            glDisable(GL_DEPTH_BOUNDS_TEST_EXT);
            checkGLcall("glDisable(GL_DEPTH_BOUNDS_TEST_EXT)");
        }
    }
    else {
        glDisable(GL_DEPTH_BOUNDS_TEST_EXT);
        checkGLcall("glDisable(GL_DEPTH_BOUNDS_TEST_EXT)");
    }

    state_tessellation(context, state, STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION));
}

static void state_wrapu(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_WRAPU])
        FIXME("Render state WINED3D_RS_WRAPU not implemented yet.\n");
}

static void state_wrapv(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_WRAPV])
        FIXME("Render state WINED3D_RS_WRAPV not implemented yet.\n");
}

static void state_monoenable(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_MONOENABLE])
        FIXME("Render state WINED3D_RS_MONOENABLE not implemented yet.\n");
}

static void state_rop2(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_ROP2])
        FIXME("Render state WINED3D_RS_ROP2 not implemented yet.\n");
}

static void state_planemask(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_PLANEMASK])
        FIXME("Render state WINED3D_RS_PLANEMASK not implemented yet.\n");
}

static void state_subpixel(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_SUBPIXEL])
        FIXME("Render state WINED3D_RS_SUBPIXEL not implemented yet.\n");
}

static void state_subpixelx(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_SUBPIXELX])
        FIXME("Render state WINED3D_RS_SUBPIXELX not implemented yet.\n");
}

static void state_stippleenable(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_STIPPLEENABLE])
        FIXME("Render state WINED3D_RS_STIPPLEENABLE not implemented yet.\n");
}

static void state_mipmaplodbias(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_MIPMAPLODBIAS])
        FIXME("Render state WINED3D_RS_MIPMAPLODBIAS not implemented yet.\n");
}

static void state_anisotropy(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_ANISOTROPY])
        FIXME("Render state WINED3D_RS_ANISOTROPY not implemented yet.\n");
}

static void state_flushbatch(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_FLUSHBATCH])
        FIXME("Render state WINED3D_RS_FLUSHBATCH not implemented yet.\n");
}

static void state_translucentsi(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_TRANSLUCENTSORTINDEPENDENT])
        FIXME("Render state WINED3D_RS_TRANSLUCENTSORTINDEPENDENT not implemented yet.\n");
}

static void state_extents(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_EXTENTS])
        FIXME("Render state WINED3D_RS_EXTENTS not implemented yet.\n");
}

static void state_ckeyblend(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_COLORKEYBLENDENABLE])
        FIXME("Render state WINED3D_RS_COLORKEYBLENDENABLE not implemented yet.\n");
}

static void state_swvp(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_SOFTWAREVERTEXPROCESSING])
        FIXME("Software vertex processing not implemented.\n");
}

static void get_src_and_opr(DWORD arg, BOOL is_alpha, GLenum* source, GLenum* operand) {
    /* The WINED3DTA_ALPHAREPLICATE flag specifies the alpha component of the
    * input should be used for all input components. The WINED3DTA_COMPLEMENT
    * flag specifies the complement of the input should be used. */
    BOOL from_alpha = is_alpha || arg & WINED3DTA_ALPHAREPLICATE;
    BOOL complement = arg & WINED3DTA_COMPLEMENT;

    /* Calculate the operand */
    if (complement) {
        if (from_alpha) *operand = GL_ONE_MINUS_SRC_ALPHA;
        else *operand = GL_ONE_MINUS_SRC_COLOR;
    } else {
        if (from_alpha) *operand = GL_SRC_ALPHA;
        else *operand = GL_SRC_COLOR;
    }

    /* Calculate the source */
    switch (arg & WINED3DTA_SELECTMASK) {
        case WINED3DTA_CURRENT: *source = GL_PREVIOUS_EXT; break;
        case WINED3DTA_DIFFUSE: *source = GL_PRIMARY_COLOR_EXT; break;
        case WINED3DTA_TEXTURE: *source = GL_TEXTURE; break;
        case WINED3DTA_TFACTOR: *source = GL_CONSTANT_EXT; break;
        case WINED3DTA_SPECULAR:
            /*
            * According to the GL_ARB_texture_env_combine specs, SPECULAR is
            * 'Secondary color' and isn't supported until base GL supports it
            * There is no concept of temp registers as far as I can tell
            */
            FIXME("Unhandled texture arg WINED3DTA_SPECULAR\n");
            *source = GL_TEXTURE;
            break;
        default:
            FIXME("Unrecognized texture arg %#x\n", arg);
            *source = GL_TEXTURE;
            break;
    }
}

/* Setup the texture operations texture stage states */
static void set_tex_op(const struct wined3d_gl_info *gl_info, const struct wined3d_state *state,
        BOOL isAlpha, int Stage, WINED3DTEXTUREOP op, DWORD arg1, DWORD arg2, DWORD arg3)
{
    GLenum src1, src2, src3;
    GLenum opr1, opr2, opr3;
    GLenum comb_target;
    GLenum src0_target, src1_target, src2_target;
    GLenum opr0_target, opr1_target, opr2_target;
    GLenum scal_target;
    GLenum opr=0, invopr, src3_target, opr3_target;
    BOOL Handled = FALSE;

    TRACE("Alpha?(%d), Stage:%d Op(%s), a1(%d), a2(%d), a3(%d)\n", isAlpha, Stage, debug_d3dtop(op), arg1, arg2, arg3);

    /* This is called by a state handler which has the gl lock held and a context for the thread */

        /* Note: Operations usually involve two ars, src0 and src1 and are operations of
    the form (a1 <operation> a2). However, some of the more complex operations
    take 3 parameters. Instead of the (sensible) addition of a3, Microsoft added
    in a third parameter called a0. Therefore these are operations of the form
    a0 <operation> a1 <operation> a2, i.e., the new parameter goes to the front.

    However, below we treat the new (a0) parameter as src2/opr2, so in the actual
    functions below, expect their syntax to differ slightly to those listed in the
    manuals, i.e., replace arg1 with arg3, arg2 with arg1 and arg3 with arg2
    This affects WINED3DTOP_MULTIPLYADD and WINED3DTOP_LERP                     */

    if (isAlpha)
    {
        comb_target = GL_COMBINE_ALPHA;
        src0_target = GL_SOURCE0_ALPHA;
        src1_target = GL_SOURCE1_ALPHA;
        src2_target = GL_SOURCE2_ALPHA;
        opr0_target = GL_OPERAND0_ALPHA;
        opr1_target = GL_OPERAND1_ALPHA;
        opr2_target = GL_OPERAND2_ALPHA;
        scal_target = GL_ALPHA_SCALE;
    }
    else
    {
        comb_target = GL_COMBINE_RGB;
        src0_target = GL_SOURCE0_RGB;
        src1_target = GL_SOURCE1_RGB;
        src2_target = GL_SOURCE2_RGB;
        opr0_target = GL_OPERAND0_RGB;
        opr1_target = GL_OPERAND1_RGB;
        opr2_target = GL_OPERAND2_RGB;
        scal_target = GL_RGB_SCALE;
    }

        /* If a texture stage references an invalid texture unit the stage just
        * passes through the result from the previous stage */
    if (is_invalid_op(state, Stage, op, arg1, arg2, arg3))
    {
        arg1 = WINED3DTA_CURRENT;
        op = WINED3DTOP_SELECTARG1;
    }

    if (isAlpha && !state->textures[Stage] && arg1 == WINED3DTA_TEXTURE)
    {
        get_src_and_opr(WINED3DTA_DIFFUSE, isAlpha, &src1, &opr1);
    } else {
        get_src_and_opr(arg1, isAlpha, &src1, &opr1);
    }
    get_src_and_opr(arg2, isAlpha, &src2, &opr2);
    get_src_and_opr(arg3, isAlpha, &src3, &opr3);

    TRACE("ct(%x), 1:(%x,%x), 2:(%x,%x), 3:(%x,%x)\n", comb_target, src1, opr1, src2, opr2, src3, opr3);

    Handled = TRUE; /* Assume will be handled */

    /* Other texture operations require special extensions: */
    if (gl_info->supported[NV_TEXTURE_ENV_COMBINE4])
    {
        if (isAlpha) {
            opr = GL_SRC_ALPHA;
            invopr = GL_ONE_MINUS_SRC_ALPHA;
            src3_target = GL_SOURCE3_ALPHA_NV;
            opr3_target = GL_OPERAND3_ALPHA_NV;
        } else {
            opr = GL_SRC_COLOR;
            invopr = GL_ONE_MINUS_SRC_COLOR;
            src3_target = GL_SOURCE3_RGB_NV;
            opr3_target = GL_OPERAND3_RGB_NV;
        }
        switch (op) {
            case WINED3DTOP_DISABLE: /* Only for alpha */
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_REPLACE");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, GL_PREVIOUS_EXT);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, GL_SRC_ALPHA);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src2_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, opr");
                break;
                case WINED3DTOP_SELECTARG1:                                          /* = a1 * 1 + 0 * 0 */
                case WINED3DTOP_SELECTARG2:                                          /* = a2 * 1 + 0 * 0 */
                    glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                    checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                    if (op == WINED3DTOP_SELECTARG1) {
                        glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                        checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                        glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                        checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                    } else {
                        glTexEnvi(GL_TEXTURE_ENV, src0_target, src2);
                        checkGLcall("GL_TEXTURE_ENV, src0_target, src2");
                        glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr2);
                        checkGLcall("GL_TEXTURE_ENV, opr0_target, opr2");
                    }
                    glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_ZERO);
                    checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                    glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                    checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                    glTexEnvi(GL_TEXTURE_ENV, src2_target, GL_ZERO);
                    checkGLcall("GL_TEXTURE_ENV, src2_target, GL_ZERO");
                    glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr);
                    checkGLcall("GL_TEXTURE_ENV, opr2_target, opr");
                    glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_ZERO);
                    checkGLcall("GL_TEXTURE_ENV, src3_target, GL_ZERO");
                    glTexEnvi(GL_TEXTURE_ENV, opr3_target, opr);
                    checkGLcall("GL_TEXTURE_ENV, opr3_target, opr");
                    break;

            case WINED3DTOP_MODULATE:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD"); /* Add = a0*a1 + a2*a3 */
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;
            case WINED3DTOP_MODULATE2X:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD"); /* Add = a0*a1 + a2*a3 */
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 2);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 2");
                break;
            case WINED3DTOP_MODULATE4X:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD"); /* Add = a0*a1 + a2*a3 */
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 4);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 4");
                break;

            case WINED3DTOP_ADD:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;

            case WINED3DTOP_ADDSIGNED:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD_SIGNED);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD_SIGNED");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;

            case WINED3DTOP_ADDSIGNED2X:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD_SIGNED);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD_SIGNED");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 2);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 2");
                break;

            case WINED3DTOP_ADDSMOOTH:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src3_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_ONE_MINUS_SRC_COLOR; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_SRC_COLOR; break;
                    case GL_SRC_ALPHA: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_ALPHA: opr = GL_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;

            case WINED3DTOP_BLENDDIFFUSEALPHA:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_PRIMARY_COLOR);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_PRIMARY_COLOR");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_PRIMARY_COLOR);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_PRIMARY_COLOR");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, GL_ONE_MINUS_SRC_ALPHA);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, GL_ONE_MINUS_SRC_ALPHA");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;
            case WINED3DTOP_BLENDTEXTUREALPHA:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_TEXTURE);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_TEXTURE");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_TEXTURE);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_TEXTURE");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, GL_ONE_MINUS_SRC_ALPHA);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, GL_ONE_MINUS_SRC_ALPHA");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;
            case WINED3DTOP_BLENDFACTORALPHA:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_CONSTANT);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_CONSTANT");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_CONSTANT);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_CONSTANT");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, GL_ONE_MINUS_SRC_ALPHA);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, GL_ONE_MINUS_SRC_ALPHA");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;
            case WINED3DTOP_BLENDTEXTUREALPHAPM:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_TEXTURE);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_TEXTURE");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, GL_ONE_MINUS_SRC_ALPHA);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, GL_ONE_MINUS_SRC_ALPHA");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;
            case WINED3DTOP_MODULATEALPHA_ADDCOLOR:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");  /* Add = a0*a1 + a2*a3 */
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);        /*   a0 = src1/opr1    */
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");    /*   a1 = 1 (see docs) */
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);        /*   a2 = arg2         */
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");     /*  a3 = src1 alpha   */
                glTexEnvi(GL_TEXTURE_ENV, src3_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src3_target, src1");
                switch (opr) {
                    case GL_SRC_COLOR: opr = GL_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;
            case WINED3DTOP_MODULATECOLOR_ADDALPHA:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;
            case WINED3DTOP_MODULATEINVALPHA_ADDCOLOR:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src3_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_SRC_ALPHA; break;
                    case GL_SRC_ALPHA: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_ALPHA: opr = GL_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;
            case WINED3DTOP_MODULATEINVCOLOR_ADDALPHA:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_ONE_MINUS_SRC_COLOR; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_SRC_COLOR; break;
                    case GL_SRC_ALPHA: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_ALPHA: opr = GL_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src3_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;
            case WINED3DTOP_MULTIPLYADD:
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src3);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr3);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, GL_ZERO);
                checkGLcall("GL_TEXTURE_ENV, src1_target, GL_ZERO");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, invopr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, invopr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, src3_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src3_target, src3");
                glTexEnvi(GL_TEXTURE_ENV, opr3_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr3_target, opr3");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
                break;

            case WINED3DTOP_BUMPENVMAP:
            {
            }

            case WINED3DTOP_BUMPENVMAPLUMINANCE:
                FIXME("Implement bump environment mapping in GL_NV_texture_env_combine4 path\n");

            default:
                Handled = FALSE;
        }
        if (Handled) {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE4_NV);
            checkGLcall("GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE4_NV");

            return;
        }
    } /* GL_NV_texture_env_combine4 */

    Handled = TRUE; /* Again, assume handled */
    switch (op) {
        case WINED3DTOP_DISABLE: /* Only for alpha */
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_REPLACE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_REPLACE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, GL_PREVIOUS_EXT);
            checkGLcall("GL_TEXTURE_ENV, src0_target, GL_PREVIOUS_EXT");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, GL_SRC_ALPHA);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, GL_SRC_ALPHA");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_SELECTARG1:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_REPLACE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_REPLACE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_SELECTARG2:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_REPLACE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_REPLACE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_MODULATE:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_MODULATE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_MODULATE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_MODULATE2X:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_MODULATE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_MODULATE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 2);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 2");
            break;
        case WINED3DTOP_MODULATE4X:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_MODULATE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_MODULATE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 4);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 4");
            break;
        case WINED3DTOP_ADD:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_ADDSIGNED:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD_SIGNED);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD_SIGNED");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_ADDSIGNED2X:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_ADD_SIGNED);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_ADD_SIGNED");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 2);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 2");
            break;
        case WINED3DTOP_SUBTRACT:
            if (gl_info->supported[ARB_TEXTURE_ENV_COMBINE])
            {
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_SUBTRACT);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_SUBTRACT");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            } else {
                FIXME("This version of opengl does not support GL_SUBTRACT\n");
            }
            break;

        case WINED3DTOP_BLENDDIFFUSEALPHA:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_INTERPOLATE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_INTERPOLATE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, src2_target, GL_PRIMARY_COLOR);
            checkGLcall("GL_TEXTURE_ENV, src2_target, GL_PRIMARY_COLOR");
            glTexEnvi(GL_TEXTURE_ENV, opr2_target, GL_SRC_ALPHA);
            checkGLcall("GL_TEXTURE_ENV, opr2_target, GL_SRC_ALPHA");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_BLENDTEXTUREALPHA:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_INTERPOLATE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_INTERPOLATE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, src2_target, GL_TEXTURE);
            checkGLcall("GL_TEXTURE_ENV, src2_target, GL_TEXTURE");
            glTexEnvi(GL_TEXTURE_ENV, opr2_target, GL_SRC_ALPHA);
            checkGLcall("GL_TEXTURE_ENV, opr2_target, GL_SRC_ALPHA");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_BLENDFACTORALPHA:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_INTERPOLATE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_INTERPOLATE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, src2_target, GL_CONSTANT);
            checkGLcall("GL_TEXTURE_ENV, src2_target, GL_CONSTANT");
            glTexEnvi(GL_TEXTURE_ENV, opr2_target, GL_SRC_ALPHA);
            checkGLcall("GL_TEXTURE_ENV, opr2_target, GL_SRC_ALPHA");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_BLENDCURRENTALPHA:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_INTERPOLATE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_INTERPOLATE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, src2_target, GL_PREVIOUS);
            checkGLcall("GL_TEXTURE_ENV, src2_target, GL_PREVIOUS");
            glTexEnvi(GL_TEXTURE_ENV, opr2_target, GL_SRC_ALPHA);
            checkGLcall("GL_TEXTURE_ENV, opr2_target, GL_SRC_ALPHA");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_DOTPRODUCT3:
            if (gl_info->supported[ARB_TEXTURE_ENV_DOT3])
            {
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_DOT3_RGBA_ARB);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_DOT3_RGBA_ARB");
            }
            else if (gl_info->supported[EXT_TEXTURE_ENV_DOT3])
            {
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_DOT3_RGBA_EXT);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_DOT3_RGBA_EXT");
            } else {
                FIXME("This version of opengl does not support GL_DOT3\n");
            }
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_LERP:
            glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_INTERPOLATE);
            checkGLcall("GL_TEXTURE_ENV, comb_target, GL_INTERPOLATE");
            glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
            checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
            glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
            checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
            glTexEnvi(GL_TEXTURE_ENV, src1_target, src2);
            checkGLcall("GL_TEXTURE_ENV, src1_target, src2");
            glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr2);
            checkGLcall("GL_TEXTURE_ENV, opr1_target, opr2");
            glTexEnvi(GL_TEXTURE_ENV, src2_target, src3);
            checkGLcall("GL_TEXTURE_ENV, src2_target, src3");
            glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr3);
            checkGLcall("GL_TEXTURE_ENV, opr2_target, opr3");
            glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
            checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            break;
        case WINED3DTOP_ADDSMOOTH:
            if (gl_info->supported[ATI_TEXTURE_ENV_COMBINE3])
            {
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_ONE_MINUS_SRC_COLOR; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_SRC_COLOR; break;
                    case GL_SRC_ALPHA: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_ALPHA: opr = GL_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src1_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            } else
                Handled = FALSE;
                break;
        case WINED3DTOP_BLENDTEXTUREALPHAPM:
            if (gl_info->supported[ATI_TEXTURE_ENV_COMBINE3])
            {
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, GL_TEXTURE);
                checkGLcall("GL_TEXTURE_ENV, src0_target, GL_TEXTURE");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, GL_ONE_MINUS_SRC_ALPHA);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, GL_ONE_MINUS_SRC_APHA");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src1_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            } else
                Handled = FALSE;
                break;
        case WINED3DTOP_MODULATEALPHA_ADDCOLOR:
            if (gl_info->supported[ATI_TEXTURE_ENV_COMBINE3])
            {
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_SRC_ALPHA: opr = GL_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_ALPHA: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src1_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            } else
                Handled = FALSE;
                break;
        case WINED3DTOP_MODULATECOLOR_ADDALPHA:
            if (gl_info->supported[ATI_TEXTURE_ENV_COMBINE3])
            {
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src1_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_SRC_ALPHA: opr = GL_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_ALPHA: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            } else
                Handled = FALSE;
                break;
        case WINED3DTOP_MODULATEINVALPHA_ADDCOLOR:
            if (gl_info->supported[ATI_TEXTURE_ENV_COMBINE3])
            {
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_SRC_ALPHA; break;
                    case GL_SRC_ALPHA: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_ALPHA: opr = GL_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src1_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            } else
                Handled = FALSE;
                break;
        case WINED3DTOP_MODULATEINVCOLOR_ADDALPHA:
            if (gl_info->supported[ATI_TEXTURE_ENV_COMBINE3])
            {
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_ONE_MINUS_SRC_COLOR; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_SRC_COLOR; break;
                    case GL_SRC_ALPHA: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_ALPHA: opr = GL_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src1_target, src1");
                switch (opr1) {
                    case GL_SRC_COLOR: opr = GL_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_COLOR: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                    case GL_SRC_ALPHA: opr = GL_SRC_ALPHA; break;
                    case GL_ONE_MINUS_SRC_ALPHA: opr = GL_ONE_MINUS_SRC_ALPHA; break;
                }
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, opr");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            } else
                Handled = FALSE;
                break;
        case WINED3DTOP_MULTIPLYADD:
            if (gl_info->supported[ATI_TEXTURE_ENV_COMBINE3])
            {
                glTexEnvi(GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI);
                checkGLcall("GL_TEXTURE_ENV, comb_target, GL_MODULATE_ADD_ATI");
                glTexEnvi(GL_TEXTURE_ENV, src0_target, src1);
                checkGLcall("GL_TEXTURE_ENV, src0_target, src1");
                glTexEnvi(GL_TEXTURE_ENV, opr0_target, opr1);
                checkGLcall("GL_TEXTURE_ENV, opr0_target, opr1");
                glTexEnvi(GL_TEXTURE_ENV, src1_target, src3);
                checkGLcall("GL_TEXTURE_ENV, src1_target, src3");
                glTexEnvi(GL_TEXTURE_ENV, opr1_target, opr3);
                checkGLcall("GL_TEXTURE_ENV, opr1_target, opr3");
                glTexEnvi(GL_TEXTURE_ENV, src2_target, src2);
                checkGLcall("GL_TEXTURE_ENV, src2_target, src2");
                glTexEnvi(GL_TEXTURE_ENV, opr2_target, opr2);
                checkGLcall("GL_TEXTURE_ENV, opr2_target, opr2");
                glTexEnvi(GL_TEXTURE_ENV, scal_target, 1);
                checkGLcall("GL_TEXTURE_ENV, scal_target, 1");
            } else
                Handled = FALSE;
                break;
        case WINED3DTOP_BUMPENVMAPLUMINANCE:
        case WINED3DTOP_BUMPENVMAP:
            if (gl_info->supported[NV_TEXTURE_SHADER2])
            {
                /* Technically texture shader support without register combiners is possible, but not expected to occur
                 * on real world cards, so for now a fixme should be enough
                 */
                FIXME("Implement bump mapping with GL_NV_texture_shader in non register combiner path\n");
            }
        default:
            Handled = FALSE;
    }

    if (Handled) {
        BOOL  combineOK = TRUE;
        if (gl_info->supported[NV_TEXTURE_ENV_COMBINE4])
        {
            DWORD op2;

            if (isAlpha)
                op2 = state->texture_states[Stage][WINED3DTSS_COLOROP];
            else
                op2 = state->texture_states[Stage][WINED3DTSS_ALPHAOP];

            /* Note: If COMBINE4 in effect can't go back to combine! */
            switch (op2) {
                case WINED3DTOP_ADDSMOOTH:
                case WINED3DTOP_BLENDTEXTUREALPHAPM:
                case WINED3DTOP_MODULATEALPHA_ADDCOLOR:
                case WINED3DTOP_MODULATECOLOR_ADDALPHA:
                case WINED3DTOP_MODULATEINVALPHA_ADDCOLOR:
                case WINED3DTOP_MODULATEINVCOLOR_ADDALPHA:
                case WINED3DTOP_MULTIPLYADD:
                    /* Ignore those implemented in both cases */
                    switch (op) {
                        case WINED3DTOP_SELECTARG1:
                        case WINED3DTOP_SELECTARG2:
                            combineOK = FALSE;
                            Handled   = FALSE;
                            break;
                        default:
                            FIXME("Can't use COMBINE4 and COMBINE together, thisop=%s, otherop=%s, isAlpha(%d)\n", debug_d3dtop(op), debug_d3dtop(op2), isAlpha);
                            return;
                    }
            }
        }

        if (combineOK)
        {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            checkGLcall("GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE");

            return;
        }
    }

    /* After all the extensions, if still unhandled, report fixme */
    FIXME("Unhandled texture operation %s\n", debug_d3dtop(op));
}


static void tex_colorop(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD stage = (state_id - STATE_TEXTURESTAGE(0, 0)) / (WINED3D_HIGHEST_TEXTURE_STATE + 1);
    const struct wined3d_device *device = context->swapchain->device;
    BOOL tex_used = device->fixed_function_usage_map & (1 << stage);
    DWORD mapped_stage = device->texUnitMap[stage];
    const struct wined3d_gl_info *gl_info = context->gl_info;

    TRACE("Setting color op for stage %d\n", stage);

    /* Using a pixel shader? Don't care for anything here, the shader applying does it */
    if (use_ps(state)) return;

    if (stage != mapped_stage) WARN("Using non 1:1 mapping: %d -> %d!\n", stage, mapped_stage);

    if (mapped_stage != WINED3D_UNMAPPED_STAGE)
    {
        if (tex_used && mapped_stage >= gl_info->limits.textures)
        {
            FIXME("Attempt to enable unsupported stage!\n");
            return;
        }
        context_active_texture(context, gl_info, mapped_stage);
    }

    if (stage >= state->lowest_disabled_stage)
    {
        TRACE("Stage disabled\n");
        if (mapped_stage != WINED3D_UNMAPPED_STAGE)
        {
            /* Disable everything here */
            glDisable(GL_TEXTURE_2D);
            checkGLcall("glDisable(GL_TEXTURE_2D)");
            glDisable(GL_TEXTURE_3D);
            checkGLcall("glDisable(GL_TEXTURE_3D)");
            if (gl_info->supported[ARB_TEXTURE_CUBE_MAP])
            {
                glDisable(GL_TEXTURE_CUBE_MAP_ARB);
                checkGLcall("glDisable(GL_TEXTURE_CUBE_MAP_ARB)");
            }
            if (gl_info->supported[ARB_TEXTURE_RECTANGLE])
            {
                glDisable(GL_TEXTURE_RECTANGLE_ARB);
                checkGLcall("glDisable(GL_TEXTURE_RECTANGLE_ARB)");
            }
        }
        /* All done */
        return;
    }

    /* The sampler will also activate the correct texture dimensions, so no
     * need to do it here if the sampler for this stage is dirty. */
    if (!isStateDirty(context, STATE_SAMPLER(stage)) && tex_used)
        texture_activate_dimensions(state->textures[stage], gl_info);

    set_tex_op(gl_info, state, FALSE, stage,
            state->texture_states[stage][WINED3DTSS_COLOROP],
            state->texture_states[stage][WINED3DTSS_COLORARG1],
            state->texture_states[stage][WINED3DTSS_COLORARG2],
            state->texture_states[stage][WINED3DTSS_COLORARG0]);
}

void tex_alphaop(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD stage = (state_id - STATE_TEXTURESTAGE(0, 0)) / (WINED3D_HIGHEST_TEXTURE_STATE + 1);
    const struct wined3d_device *device = context->swapchain->device;
    BOOL tex_used = device->fixed_function_usage_map & (1 << stage);
    DWORD mapped_stage = device->texUnitMap[stage];
    const struct wined3d_gl_info *gl_info = context->gl_info;
    DWORD op, arg1, arg2, arg0;

    TRACE("Setting alpha op for stage %d\n", stage);
    /* Do not care for enabled / disabled stages, just assign the settings. colorop disables / enables required stuff */
    if (mapped_stage != WINED3D_UNMAPPED_STAGE)
    {
        if (tex_used && mapped_stage >= gl_info->limits.textures)
        {
            FIXME("Attempt to enable unsupported stage!\n");
            return;
        }
        context_active_texture(context, gl_info, mapped_stage);
    }

    op = state->texture_states[stage][WINED3DTSS_ALPHAOP];
    arg1 = state->texture_states[stage][WINED3DTSS_ALPHAARG1];
    arg2 = state->texture_states[stage][WINED3DTSS_ALPHAARG2];
    arg0 = state->texture_states[stage][WINED3DTSS_ALPHAARG0];

    if (state->render_states[WINED3D_RS_COLORKEYENABLE] && !stage && state->textures[0])
    {
        struct wined3d_texture *texture = state->textures[0];
        GLenum texture_dimensions = texture->target;

        if (texture_dimensions == GL_TEXTURE_2D || texture_dimensions == GL_TEXTURE_RECTANGLE_ARB)
        {
            struct wined3d_surface *surf = surface_from_resource(texture->sub_resources[0]);

            if (surf->CKeyFlags & WINEDDSD_CKSRCBLT && !surf->resource.format->alpha_mask)
            {
                /* Color keying needs to pass alpha values from the texture through to have the alpha test work
                 * properly. On the other hand applications can still use texture combiners apparently. This code
                 * takes care that apps cannot remove the texture's alpha channel entirely.
                 *
                 * The fixup is required for Prince of Persia 3D(prison bars), while Moto racer 2 requires
                 * D3DTOP_MODULATE to work on color keyed surfaces. Aliens vs Predator 1 uses color keyed textures
                 * and alpha component of diffuse color to draw things like translucent text and perform other
                 * blending effects.
                 *
                 * Aliens vs Predator 1 relies on diffuse alpha having an effect, so it cannot be ignored. To
                 * provide the behavior expected by the game, while emulating the colorkey, diffuse alpha must be
                 * modulated with texture alpha. OTOH, Moto racer 2 at some points sets alphaop/alphaarg to
                 * SELECTARG/CURRENT, yet puts garbage in diffuse alpha (zeroes). This works on native, because the
                 * game disables alpha test and alpha blending. Alpha test is overwritten by wine's for purposes of
                 * color-keying though, so this will lead to missing geometry if texture alpha is modulated (pixels
                 * fail alpha test). To get around this, ALPHABLENDENABLE state is checked: if the app enables alpha
                 * blending, it can be expected to provide meaningful values in diffuse alpha, so it should be
                 * modulated with texture alpha; otherwise, selecting diffuse alpha is ignored in favour of texture
                 * alpha.
                 *
                 * What to do with multitexturing? So far no app has been found that uses color keying with
                 * multitexturing */
                if (op == WINED3DTOP_DISABLE)
                {
                    arg1 = WINED3DTA_TEXTURE;
                    op = WINED3DTOP_SELECTARG1;
                }
                else if(op == WINED3DTOP_SELECTARG1 && arg1 != WINED3DTA_TEXTURE)
                {
                    if (state->render_states[WINED3D_RS_ALPHABLENDENABLE])
                    {
                        arg2 = WINED3DTA_TEXTURE;
                        op = WINED3DTOP_MODULATE;
                    }
                    else arg1 = WINED3DTA_TEXTURE;
                }
                else if(op == WINED3DTOP_SELECTARG2 && arg2 != WINED3DTA_TEXTURE)
                {
                    if (state->render_states[WINED3D_RS_ALPHABLENDENABLE])
                    {
                        arg1 = WINED3DTA_TEXTURE;
                        op = WINED3DTOP_MODULATE;
                    }
                    else arg2 = WINED3DTA_TEXTURE;
                }
            }
        }
    }

    /* tex_alphaop is shared between the ffp and nvrc because the difference only comes down to
     * this if block here, and the other code(color keying, texture unit selection) are the same
     */
    TRACE("Setting alpha op for stage %d\n", stage);
    if (gl_info->supported[NV_REGISTER_COMBINERS])
    {
        set_tex_op_nvrc(gl_info, state, TRUE, stage, op, arg1, arg2, arg0,
                mapped_stage, state->texture_states[stage][WINED3DTSS_RESULTARG]);
    }
    else
    {
        set_tex_op(gl_info, state, TRUE, stage, op, arg1, arg2, arg0);
    }
}

static void transform_texture(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD texUnit = (state_id - STATE_TEXTURESTAGE(0, 0)) / (WINED3D_HIGHEST_TEXTURE_STATE + 1);
    const struct wined3d_device *device = context->swapchain->device;
    const struct wined3d_gl_info *gl_info = context->gl_info;
    DWORD mapped_stage = device->texUnitMap[texUnit];
    BOOL generated;
    int coordIdx;

    /* Ignore this when a vertex shader is used, or if the streams aren't sorted out yet */
    if (use_vs(state) || isStateDirty(context, STATE_VDECL))
    {
        TRACE("Using a vertex shader, or stream sources not sorted out yet, skipping\n");
        return;
    }

    if (mapped_stage == WINED3D_UNMAPPED_STAGE) return;
    if (mapped_stage >= gl_info->limits.textures) return;

    context_active_texture(context, gl_info, mapped_stage);
    generated = (state->texture_states[texUnit][WINED3DTSS_TEXCOORDINDEX] & 0xffff0000) != WINED3DTSS_TCI_PASSTHRU;
    coordIdx = min(state->texture_states[texUnit][WINED3DTSS_TEXCOORDINDEX & 0x0000ffff], MAX_TEXTURES - 1);

    set_texture_matrix(&state->transforms[WINED3DTS_TEXTURE0 + texUnit].u.m[0][0],
            state->texture_states[texUnit][WINED3DTSS_TEXTURETRANSFORMFLAGS],
            generated, context->last_was_rhw,
            device->strided_streams.use_map & (1 << (WINED3D_FFP_TEXCOORD0 + coordIdx))
            ? device->strided_streams.elements[WINED3D_FFP_TEXCOORD0 + coordIdx].format->id
            : WINED3DFMT_UNKNOWN,
            device->frag_pipe->ffp_proj_control);

    /* The sampler applying function calls us if this changes */
    if ((context->lastWasPow2Texture & (1 << texUnit)) && state->textures[texUnit])
    {
        if(generated) {
            FIXME("Non-power2 texture being used with generated texture coords\n");
        }
        /* NP2 texcoord fixup is implemented for pixelshaders so only enable the
           fixed-function-pipeline fixup via pow2Matrix when no PS is used. */
        if (!use_ps(state))
        {
            TRACE("Non power two matrix multiply fixup\n");
            glMultMatrixf(state->textures[texUnit]->pow2_matrix);
        }
    }
}

static void unload_tex_coords(const struct wined3d_gl_info *gl_info)
{
    unsigned int texture_idx;

    for (texture_idx = 0; texture_idx < gl_info->limits.texture_coords; ++texture_idx)
    {
        GL_EXTCALL(glClientActiveTextureARB(GL_TEXTURE0_ARB + texture_idx));
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
}

static void load_tex_coords(const struct wined3d_context *context, const struct wined3d_stream_info *si,
        GLuint *curVBO, const struct wined3d_state *state)
{
    const struct wined3d_device *device = context->swapchain->device;
    const struct wined3d_gl_info *gl_info = context->gl_info;
    unsigned int mapped_stage = 0;
    unsigned int textureNo = 0;

    for (textureNo = 0; textureNo < gl_info->limits.texture_stages; ++textureNo)
    {
        int coordIdx = state->texture_states[textureNo][WINED3DTSS_TEXCOORDINDEX];

        mapped_stage = device->texUnitMap[textureNo];
        if (mapped_stage == WINED3D_UNMAPPED_STAGE) continue;

        if (mapped_stage >= gl_info->limits.texture_coords)
        {
            FIXME("Attempted to load unsupported texture coordinate %u\n", mapped_stage);
            continue;
        }

        if (coordIdx < MAX_TEXTURES && (si->use_map & (1 << (WINED3D_FFP_TEXCOORD0 + coordIdx))))
        {
            const struct wined3d_stream_info_element *e = &si->elements[WINED3D_FFP_TEXCOORD0 + coordIdx];

            TRACE("Setting up texture %u, idx %d, coordindx %u, data {%#x:%p}.\n",
                    textureNo, mapped_stage, coordIdx, e->data.buffer_object, e->data.addr);

            if (*curVBO != e->data.buffer_object)
            {
                GL_EXTCALL(glBindBufferARB(GL_ARRAY_BUFFER_ARB, e->data.buffer_object));
                checkGLcall("glBindBufferARB");
                *curVBO = e->data.buffer_object;
            }

            GL_EXTCALL(glClientActiveTextureARB(GL_TEXTURE0_ARB + mapped_stage));
            checkGLcall("glClientActiveTextureARB");

            /* The coords to supply depend completely on the fvf / vertex shader */
            glTexCoordPointer(e->format->gl_vtx_format, e->format->gl_vtx_type, e->stride,
                    e->data.addr + state->load_base_vertex_index * e->stride);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        else
        {
            GL_EXTCALL(glMultiTexCoord4fARB(GL_TEXTURE0_ARB + mapped_stage, 0, 0, 0, 1));
        }
    }
    if (gl_info->supported[NV_REGISTER_COMBINERS])
    {
        /* The number of the mapped stages increases monotonically, so it's fine to use the last used one. */
        for (textureNo = mapped_stage + 1; textureNo < gl_info->limits.textures; ++textureNo)
        {
            GL_EXTCALL(glMultiTexCoord4fARB(GL_TEXTURE0_ARB + textureNo, 0, 0, 0, 1));
        }
    }

    checkGLcall("loadTexCoords");
}

static void tex_coordindex(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD stage = (state_id - STATE_TEXTURESTAGE(0, 0)) / (WINED3D_HIGHEST_TEXTURE_STATE + 1);
    const struct wined3d_device *device = context->swapchain->device;
    static const GLfloat s_plane[] = { 1.0f, 0.0f, 0.0f, 0.0f };
    static const GLfloat t_plane[] = { 0.0f, 1.0f, 0.0f, 0.0f };
    static const GLfloat r_plane[] = { 0.0f, 0.0f, 1.0f, 0.0f };
    static const GLfloat q_plane[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const struct wined3d_gl_info *gl_info = context->gl_info;
    DWORD mapped_stage = device->texUnitMap[stage];

    if (mapped_stage == WINED3D_UNMAPPED_STAGE)
    {
        TRACE("No texture unit mapped to stage %d. Skipping texture coordinates.\n", stage);
        return;
    }

    if (mapped_stage >= gl_info->limits.fragment_samplers)
    {
        WARN("stage %u not mapped to a valid texture unit (%u)\n", stage, mapped_stage);
        return;
    }
    context_active_texture(context, gl_info, mapped_stage);

    /* Values 0-7 are indexes into the FVF tex coords - See comments in DrawPrimitive
     *
     * FIXME: When using generated texture coordinates, the index value is used to specify the wrapping mode.
     * eg. SetTextureStageState( 0, WINED3DTSS_TEXCOORDINDEX, WINED3DTSS_TCI_CAMERASPACEPOSITION | 1 );
     * means use the vertex position (camera-space) as the input texture coordinates
     * for this texture stage, and the wrap mode set in the WINED3D_RS_WRAP1 render
     * state. We do not (yet) support the WINED3DRENDERSTATE_WRAPx values, nor tie them up
     * to the TEXCOORDINDEX value
     */
    switch (state->texture_states[stage][WINED3DTSS_TEXCOORDINDEX] & 0xffff0000)
    {
        case WINED3DTSS_TCI_PASSTHRU:
            /* Use the specified texture coordinates contained within the
             * vertex format. This value resolves to zero. */
            glDisable(GL_TEXTURE_GEN_S);
            glDisable(GL_TEXTURE_GEN_T);
            glDisable(GL_TEXTURE_GEN_R);
            glDisable(GL_TEXTURE_GEN_Q);
            checkGLcall("WINED3DTSS_TCI_PASSTHRU - Disable texgen.");
            break;

        case WINED3DTSS_TCI_CAMERASPACEPOSITION:
            /* CameraSpacePosition means use the vertex position, transformed to camera space,
             * as the input texture coordinates for this stage's texture transformation. This
             * equates roughly to EYE_LINEAR */

            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();
            glTexGenfv(GL_S, GL_EYE_PLANE, s_plane);
            glTexGenfv(GL_T, GL_EYE_PLANE, t_plane);
            glTexGenfv(GL_R, GL_EYE_PLANE, r_plane);
            glTexGenfv(GL_Q, GL_EYE_PLANE, q_plane);
            glPopMatrix();
            checkGLcall("WINED3DTSS_TCI_CAMERASPACEPOSITION - Set eye plane.");

            glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
            glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
            glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
            checkGLcall("WINED3DTSS_TCI_CAMERASPACEPOSITION - Set texgen mode.");

            glEnable(GL_TEXTURE_GEN_S);
            glEnable(GL_TEXTURE_GEN_T);
            glEnable(GL_TEXTURE_GEN_R);
            checkGLcall("WINED3DTSS_TCI_CAMERASPACEPOSITION - Enable texgen.");

            break;

        case WINED3DTSS_TCI_CAMERASPACENORMAL:
            /* Note that NV_TEXGEN_REFLECTION support is implied when
             * ARB_TEXTURE_CUBE_MAP is supported */
            if (!gl_info->supported[NV_TEXGEN_REFLECTION])
            {
                FIXME("WINED3DTSS_TCI_CAMERASPACENORMAL not supported.\n");
                break;
            }

            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();
            glTexGenfv(GL_S, GL_EYE_PLANE, s_plane);
            glTexGenfv(GL_T, GL_EYE_PLANE, t_plane);
            glTexGenfv(GL_R, GL_EYE_PLANE, r_plane);
            glTexGenfv(GL_Q, GL_EYE_PLANE, q_plane);
            glPopMatrix();
            checkGLcall("WINED3DTSS_TCI_CAMERASPACENORMAL - Set eye plane.");

            glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_NV);
            glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_NV);
            glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_NV);
            checkGLcall("WINED3DTSS_TCI_CAMERASPACENORMAL - Set texgen mode.");

            glEnable(GL_TEXTURE_GEN_S);
            glEnable(GL_TEXTURE_GEN_T);
            glEnable(GL_TEXTURE_GEN_R);
            checkGLcall("WINED3DTSS_TCI_CAMERASPACENORMAL - Enable texgen.");

            break;

        case WINED3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR:
            /* Note that NV_TEXGEN_REFLECTION support is implied when
             * ARB_TEXTURE_CUBE_MAP is supported */
            if (!gl_info->supported[NV_TEXGEN_REFLECTION])
            {
                FIXME("WINED3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR not supported.\n");
                break;
            }

            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();
            glTexGenfv(GL_S, GL_EYE_PLANE, s_plane);
            glTexGenfv(GL_T, GL_EYE_PLANE, t_plane);
            glTexGenfv(GL_R, GL_EYE_PLANE, r_plane);
            glTexGenfv(GL_Q, GL_EYE_PLANE, q_plane);
            glPopMatrix();
            checkGLcall("WINED3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR - Set eye plane.");

            glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP_NV);
            glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP_NV);
            glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP_NV);
            checkGLcall("WINED3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR - Set texgen mode.");

            glEnable(GL_TEXTURE_GEN_S);
            glEnable(GL_TEXTURE_GEN_T);
            glEnable(GL_TEXTURE_GEN_R);
            checkGLcall("WINED3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR - Enable texgen.");

            break;

        case WINED3DTSS_TCI_SPHEREMAP:
            glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
            glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
            checkGLcall("WINED3DTSS_TCI_SPHEREMAP - Set texgen mode.");

            glEnable(GL_TEXTURE_GEN_S);
            glEnable(GL_TEXTURE_GEN_T);
            glDisable(GL_TEXTURE_GEN_R);
            checkGLcall("WINED3DTSS_TCI_SPHEREMAP - Enable texgen.");

            break;

        default:
            FIXME("Unhandled WINED3DTSS_TEXCOORDINDEX %#x.\n",
                    state->texture_states[stage][WINED3DTSS_TEXCOORDINDEX]);
            glDisable(GL_TEXTURE_GEN_S);
            glDisable(GL_TEXTURE_GEN_T);
            glDisable(GL_TEXTURE_GEN_R);
            glDisable(GL_TEXTURE_GEN_Q);
            checkGLcall("Disable texgen.");

            break;
    }

    /* Update the texture matrix. */
    if (!isStateDirty(context, STATE_TRANSFORM(WINED3DTS_TEXTURE0 + stage)))
        transform_texture(context, state, STATE_TEXTURESTAGE(stage, WINED3DTSS_TEXTURETRANSFORMFLAGS));

    if(!isStateDirty(context, STATE_VDECL) && context->namedArraysLoaded) {
        /* Reload the arrays if we are using fixed function arrays to reflect the selected coord input
         * source. Call loadTexCoords directly because there is no need to reparse the vertex declaration
         * and do all the things linked to it
         * TODO: Tidy that up to reload only the arrays of the changed unit
         */
        GLuint curVBO = gl_info->supported[ARB_VERTEX_BUFFER_OBJECT] ? ~0U : 0;

        unload_tex_coords(gl_info);
        load_tex_coords(context, &device->strided_streams, &curVBO, state);
    }
}

static void tex_bumpenvlscale(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD stage = (state_id - STATE_TEXTURESTAGE(0, 0)) / (WINED3D_HIGHEST_TEXTURE_STATE + 1);
    const struct wined3d_shader *ps = state->pixel_shader;

    if (ps && stage && (ps->reg_maps.luminanceparams & (1 << stage)))
    {
        /* The pixel shader has to know the luminance scale. Do a constants
         * update if it isn't scheduled anyway. */
        if (!isStateDirty(context, STATE_PIXELSHADERCONSTANT)
                && !isStateDirty(context, STATE_PIXELSHADER))
            shaderconstant(context, state, STATE_PIXELSHADERCONSTANT);
    }
}

static void sampler_texmatrix(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const DWORD sampler = state_id - STATE_SAMPLER(0);
    const struct wined3d_texture *texture = state->textures[sampler];

    TRACE("context %p, state %p, state_id %#x.\n", context, state, state_id);

    if(!texture) return;
    /* The fixed function np2 texture emulation uses the texture matrix to fix up the coordinates
     * wined3d_texture_apply_state_changes() multiplies the set matrix with a fixup matrix. Before the
     * scaling is reapplied or removed, the texture matrix has to be reapplied
     *
     * The mapped stage is already active because the sampler() function below, which is part of the
     * misc pipeline
     */
    if (sampler < MAX_TEXTURES)
    {
        const BOOL texIsPow2 = !(texture->flags & WINED3D_TEXTURE_POW2_MAT_IDENT);

        if (texIsPow2 || (context->lastWasPow2Texture & (1 << sampler)))
        {
            const struct wined3d_device *device = context->swapchain->device;

            if (texIsPow2)
                context->lastWasPow2Texture |= 1 << sampler;
            else
                context->lastWasPow2Texture &= ~(1 << sampler);

            transform_texture(context, state,
                    STATE_TEXTURESTAGE(device->texUnitMap[sampler], WINED3DTSS_TEXTURETRANSFORMFLAGS));
        }
    }
}

static void sampler(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_device *device = context->swapchain->device;
    DWORD sampler = state_id - STATE_SAMPLER(0);
    DWORD mapped_stage = device->texUnitMap[sampler];
    const struct wined3d_gl_info *gl_info = context->gl_info;
    union {
        float f;
        DWORD d;
    } tmpvalue;

    TRACE("Sampler: %d\n", sampler);
    /* Enabling and disabling texture dimensions is done by texture stage state / pixel shader setup, this function
     * only has to bind textures and set the per texture states
     */

    if (mapped_stage == WINED3D_UNMAPPED_STAGE)
    {
        TRACE("No sampler mapped to stage %d. Returning.\n", sampler);
        return;
    }

    if (mapped_stage >= gl_info->limits.combined_samplers)
    {
        return;
    }
    context_active_texture(context, gl_info, mapped_stage);

    if (state->textures[sampler])
    {
        struct wined3d_texture *texture = state->textures[sampler];
        BOOL srgb = state->sampler_states[sampler][WINED3DSAMP_SRGBTEXTURE];

        texture->texture_ops->texture_bind(texture, context, srgb);
        wined3d_texture_apply_state_changes(texture, state->sampler_states[sampler], gl_info);

        if (gl_info->supported[EXT_TEXTURE_LOD_BIAS])
        {
            tmpvalue.d = state->sampler_states[sampler][WINED3DSAMP_MIPMAPLODBIAS];
            glTexEnvf(GL_TEXTURE_FILTER_CONTROL_EXT,
                      GL_TEXTURE_LOD_BIAS_EXT,
                      tmpvalue.f);
            checkGLcall("glTexEnvf(GL_TEXTURE_LOD_BIAS_EXT, ...)");
        }

        if (!use_ps(state) && sampler < state->lowest_disabled_stage)
        {
            if (state->render_states[WINED3D_RS_COLORKEYENABLE] && !sampler)
            {
                /* If color keying is enabled update the alpha test, it
                 * depends on the existence of a color key in stage 0. */
                state_alpha(context, state, WINED3D_RS_COLORKEYENABLE);
            }
        }

        /* Trigger shader constant reloading (for NP2 texcoord fixup) */
        if (!(texture->flags & WINED3D_TEXTURE_POW2_MAT_IDENT))
            device->shader_backend->shader_load_np2fixup_constants(device->shader_priv, gl_info, state);
    }
    else
    {
        if (sampler < state->lowest_disabled_stage)
        {
            /* TODO: What should I do with pixel shaders here ??? */
            if (state->render_states[WINED3D_RS_COLORKEYENABLE] && !sampler)
            {
                /* If color keying is enabled update the alpha test, it
                 * depends on the existence of a color key in stage 0. */
                state_alpha(context, state, WINED3D_RS_COLORKEYENABLE);
            }
        } /* Otherwise tex_colorop disables the stage */
        context_bind_texture(context, GL_NONE, 0);
    }
}

void apply_pixelshader(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_device *device = context->swapchain->device;
    BOOL use_vshader = use_vs(state);
    BOOL use_pshader = use_ps(state);
    unsigned int i;

    if (use_pshader)
    {
        if (!context->last_was_pshader)
        {
            /* Former draw without a pixel shader, some samplers may be
             * disabled because of WINED3DTSS_COLOROP = WINED3DTOP_DISABLE
             * make sure to enable them. */
            for (i = 0; i < MAX_FRAGMENT_SAMPLERS; ++i)
            {
                if (!isStateDirty(context, STATE_SAMPLER(i)))
                    sampler(context, state, STATE_SAMPLER(i));
            }
            context->last_was_pshader = TRUE;
        }
        else
        {
            /* Otherwise all samplers were activated by the code above in
             * earlier draws, or by sampler() if a different texture was
             * bound. I don't have to do anything. */
        }
    }
    else
    {
        /* Disabled the pixel shader - color ops weren't applied while it was
         * enabled, so re-apply them. */
        for (i = 0; i < context->gl_info->limits.texture_stages; ++i)
        {
            if (!isStateDirty(context, STATE_TEXTURESTAGE(i, WINED3DTSS_COLOROP)))
                context_apply_state(context, state, STATE_TEXTURESTAGE(i, WINED3DTSS_COLOROP));
        }
        context->last_was_pshader = FALSE;
    }

    if (!isStateDirty(context, context->state_table[STATE_VSHADER].representative))
    {
        device->shader_backend->shader_select(context, use_pshader, use_vshader);

        if (!isStateDirty(context, STATE_VERTEXSHADERCONSTANT) && (use_vshader || use_pshader))
            shaderconstant(context, state, STATE_VERTEXSHADERCONSTANT);
    }
}

static void shader_bumpenvmat(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD stage = (state_id - STATE_TEXTURESTAGE(0, 0)) / (WINED3D_HIGHEST_TEXTURE_STATE + 1);
    const struct wined3d_shader *ps = state->pixel_shader;

    if (ps && stage && (ps->reg_maps.bumpmat & (1 << stage)))
    {
        /* The pixel shader has to know the bump env matrix. Do a constants
         * update if it isn't scheduled anyway. */
        if (!isStateDirty(context, STATE_PIXELSHADERCONSTANT)
                && !isStateDirty(context, STATE_PIXELSHADER))
            shaderconstant(context, state, STATE_PIXELSHADERCONSTANT);
    }
}

static void transform_world(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    /* This function is called by transform_view below if the view matrix was changed too
     *
     * Deliberately no check if the vertex declaration is dirty because the vdecl state
     * does not always update the world matrix, only on a switch between transformed
     * and untransformed draws. It *may* happen that the world matrix is set 2 times during one
     * draw, but that should be rather rare and cheaper in total.
     */
    glMatrixMode(GL_MODELVIEW);
    checkGLcall("glMatrixMode");

    if(context->last_was_rhw) {
        glLoadIdentity();
        checkGLcall("glLoadIdentity()");
    }
    else if (!context->swapchain->device->vertexBlendSW)
    {
        /* In the general case, the view matrix is the identity matrix */
        if (context->swapchain->device->view_ident)
        {
            glLoadMatrixf(&state->transforms[WINED3DTS_WORLDMATRIX(0)].u.m[0][0]);
            checkGLcall("glLoadMatrixf");
        }
        else
        {
            glLoadMatrixf(&state->transforms[WINED3DTS_VIEW].u.m[0][0]);
            checkGLcall("glLoadMatrixf");
            glMultMatrixf(&state->transforms[WINED3DTS_WORLDMATRIX(0)].u.m[0][0]);
            checkGLcall("glMultMatrixf");
        }
    } else {
        glLoadMatrixf(&state->transforms[WINED3DTS_VIEW].u.m[0][0]);
        checkGLcall("glLoadMatrixf");
    }
}

static void clipplane(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    UINT index = state_id - STATE_CLIPPLANE(0);

    if (isStateDirty(context, STATE_TRANSFORM(WINED3DTS_VIEW)) || index >= context->gl_info->limits.clipplanes)
    {
        return;
    }

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();

    /* Clip Plane settings are affected by the model view in OpenGL, the View transform in direct3d */
    if (!use_vs(state))
        glLoadMatrixf(&state->transforms[WINED3DTS_VIEW].u.m[0][0]);
    else
        /* with vertex shaders, clip planes are not transformed in direct3d,
         * in OpenGL they are still transformed by the model view.
         */
        glLoadIdentity();

    TRACE("Clipplane [%.8e, %.8e, %.8e, %.8e]\n",
            state->clip_planes[index][0],
            state->clip_planes[index][1],
            state->clip_planes[index][2],
            state->clip_planes[index][3]);
    glClipPlane(GL_CLIP_PLANE0 + index, state->clip_planes[index]);
    checkGLcall("glClipPlane");

    glPopMatrix();
}

static void transform_worldex(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    UINT matrix = state_id - STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(0));
    GLenum glMat;
    TRACE("Setting world matrix %d\n", matrix);

    if (matrix >= context->gl_info->limits.blends)
    {
        WARN("Unsupported blend matrix set\n");
        return;
    } else if(isStateDirty(context, STATE_TRANSFORM(WINED3DTS_VIEW))) {
        return;
    }

    /* GL_MODELVIEW0_ARB:  0x1700
     * GL_MODELVIEW1_ARB:  0x850a
     * GL_MODELVIEW2_ARB:  0x8722
     * GL_MODELVIEW3_ARB:  0x8723
     * etc
     * GL_MODELVIEW31_ARB: 0x873F
     */
    if(matrix == 1) glMat = GL_MODELVIEW1_ARB;
    else glMat = GL_MODELVIEW2_ARB - 2 + matrix;

    glMatrixMode(glMat);
    checkGLcall("glMatrixMode(glMat)");

    /* World matrix 0 is multiplied with the view matrix because d3d uses 3
     * matrices while gl uses only 2. To avoid weighting the view matrix
     * incorrectly it has to be multiplied into every GL modelview matrix. */
    if (context->swapchain->device->view_ident)
    {
        glLoadMatrixf(&state->transforms[WINED3DTS_WORLDMATRIX(matrix)].u.m[0][0]);
        checkGLcall("glLoadMatrixf");
    }
    else
    {
        glLoadMatrixf(&state->transforms[WINED3DTS_VIEW].u.m[0][0]);
        checkGLcall("glLoadMatrixf");
        glMultMatrixf(&state->transforms[WINED3DTS_WORLDMATRIX(matrix)].u.m[0][0]);
        checkGLcall("glMultMatrixf");
    }
}

static void state_vertexblend_w(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    enum wined3d_vertex_blend_flags f = state->render_states[WINED3D_RS_VERTEXBLEND];
    static unsigned int once;

    if (f == WINED3D_VBF_DISABLE)
        return;
    switch (f) {
        case WINED3D_VBF_0WEIGHTS:
        case WINED3D_VBF_1WEIGHTS:
        case WINED3D_VBF_2WEIGHTS:
        case WINED3D_VBF_3WEIGHTS:
            if(!once) {
                once = TRUE;
                FIXME("Vertex blending enabled, but not supported by hardware. Using software emulation.\n");
            }
            if (!context->swapchain->device->vertexBlendSW) {
                context->swapchain->device->vertexBlendSW = TRUE;
                transform_world(context, state, state_id);
            }
            break;
        case WINED3D_VBF_TWEENING:
            WARN("Vertex blend flags %#x not supported.\n", f);
            /* fall through */
        default:
            if (context->swapchain->device->vertexBlendSW) {
                context->swapchain->device->vertexBlendSW = FALSE;
                transform_world(context, state, state_id);
            }
            break;
    }
}

static void state_vertexblend(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    enum wined3d_vertex_blend_flags val = state->render_states[WINED3D_RS_VERTEXBLEND];
    struct wined3d_device *device = context->swapchain->device;
    const struct wined3d_gl_info *gl_info = context->gl_info;
    static unsigned int once;

    switch (val)
    {
        case WINED3D_VBF_1WEIGHTS:
        case WINED3D_VBF_2WEIGHTS:
        case WINED3D_VBF_3WEIGHTS:
            glEnable(GL_VERTEX_BLEND_ARB);
            checkGLcall("glEnable(GL_VERTEX_BLEND_ARB)");

            /* D3D adds one more matrix which has weight (1 - sum(weights)).
             * This is enabled at context creation with enabling
             * GL_WEIGHT_SUM_UNITY_ARB. */
            GL_EXTCALL(glVertexBlendARB(state->render_states[WINED3D_RS_VERTEXBLEND] + 1));

            if (!device->vertexBlendUsed)
            {
                unsigned int i;
                for (i = 1; i < gl_info->limits.blends; ++i)
                {
                    if (!isStateDirty(context, STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(i))))
                        transform_worldex(context, state, STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(i)));
                }
                device->vertexBlendUsed = TRUE;
            }
            break;

        case WINED3D_VBF_TWEENING:
        case WINED3D_VBF_0WEIGHTS: /* Indexed vertex blending, not supported. */
            if (!once++) FIXME("Vertex blend flags %#x not supported.\n", val);
            else WARN("Vertex blend flags %#x not supported.\n", val);
            /* Fall through. */
        case WINED3D_VBF_DISABLE:
            glDisable(GL_VERTEX_BLEND_ARB);
            checkGLcall("glDisable(GL_VERTEX_BLEND_ARB)");
            break;
    }
}

static void transform_view(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    const struct wined3d_light_info *light = NULL;
    unsigned int k;

    /* If we are changing the View matrix, reset the light and clipping planes to the new view
     * NOTE: We have to reset the positions even if the light/plane is not currently
     *       enabled, since the call to enable it will not reset the position.
     * NOTE2: Apparently texture transforms do NOT need reapplying
     */

    glMatrixMode(GL_MODELVIEW);
    checkGLcall("glMatrixMode(GL_MODELVIEW)");
    glLoadMatrixf(&state->transforms[WINED3DTS_VIEW].u.m[0][0]);
    checkGLcall("glLoadMatrixf(...)");

    /* Reset lights. TODO: Call light apply func */
    for (k = 0; k < gl_info->limits.lights; ++k)
    {
        if (!(light = state->lights[k]))
            continue;
        glLightfv(GL_LIGHT0 + light->glIndex, GL_POSITION, light->lightPosn);
        checkGLcall("glLightfv posn");
        glLightfv(GL_LIGHT0 + light->glIndex, GL_SPOT_DIRECTION, light->lightDirn);
        checkGLcall("glLightfv dirn");
    }

    /* Reset Clipping Planes  */
    for (k = 0; k < gl_info->limits.clipplanes; ++k)
    {
        if (!isStateDirty(context, STATE_CLIPPLANE(k)))
            clipplane(context, state, STATE_CLIPPLANE(k));
    }

    if(context->last_was_rhw) {
        glLoadIdentity();
        checkGLcall("glLoadIdentity()");
        /* No need to update the world matrix, the identity is fine */
        return;
    }

    /* Call the world matrix state, this will apply the combined WORLD + VIEW matrix
     * No need to do it here if the state is scheduled for update. */
    if (!isStateDirty(context, STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(0))))
        transform_world(context, state, STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(0)));

    /* Avoid looping over a number of matrices if the app never used the functionality */
    if (context->swapchain->device->vertexBlendUsed)
    {
        for (k = 1; k < gl_info->limits.blends; ++k)
        {
            if (!isStateDirty(context, STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(k))))
                transform_worldex(context, state, STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(k)));
        }
    }
}

static void transform_projection(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    glMatrixMode(GL_PROJECTION);
    checkGLcall("glMatrixMode(GL_PROJECTION)");

    /* There are a couple of additional things we have to take into account
     * here besides the projection transformation itself:
     *   - We need to flip along the y-axis in case of offscreen rendering.
     *   - OpenGL Z range is {-Wc,...,Wc} while D3D Z range is {0,...,Wc}.
     *   - D3D coordinates refer to pixel centers while GL coordinates refer
     *     to pixel corners.
     *   - D3D has a top-left filling convention. We need to maintain this
     *     even after the y-flip mentioned above.
     * In order to handle the last two points, we translate by
     * (63.0 / 128.0) / VPw and (63.0 / 128.0) / VPh. This is equivalent to
     * translating slightly less than half a pixel. We want the difference to
     * be large enough that it doesn't get lost due to rounding inside the
     * driver, but small enough to prevent it from interfering with any
     * anti-aliasing. */

    if (context->last_was_rhw)
    {
        /* Transform D3D RHW coordinates to OpenGL clip coordinates. */
        double x = state->viewport.x;
        double y = state->viewport.y;
        double w = state->viewport.width;
        double h = state->viewport.height;
        double x_scale = 2.0 / w;
        double x_offset = ((63.0 / 64.0) - (2.0 * x) - w) / w;
        double y_scale = context->render_offscreen ? 2.0 / h : 2.0 / -h;
        double y_offset = context->render_offscreen
                ? ((63.0 / 64.0) - (2.0 * y) - h) / h
                : ((63.0 / 64.0) - (2.0 * y) - h) / -h;
        const GLdouble projection[] =
        {
             x_scale,      0.0,  0.0, 0.0,
                 0.0,  y_scale,  0.0, 0.0,
                 0.0,      0.0,  2.0, 0.0,
            x_offset, y_offset, -1.0, 1.0,
        };

        glLoadMatrixd(projection);
        checkGLcall("glLoadMatrixd");
    }
    else
    {
        double y_scale = context->render_offscreen ? -1.0 : 1.0;
        double x_offset = (63.0 / 64.0) / state->viewport.width;
        double y_offset = context->render_offscreen
                ? (63.0 / 64.0) / state->viewport.height
                : -(63.0 / 64.0) / state->viewport.height;
        const GLdouble projection[] =
        {
                 1.0,      0.0,  0.0, 0.0,
                 0.0,  y_scale,  0.0, 0.0,
                 0.0,      0.0,  2.0, 0.0,
            x_offset, y_offset, -1.0, 1.0,
        };

        glLoadMatrixd(projection);
        checkGLcall("glLoadMatrixd");

        glMultMatrixf(&state->transforms[WINED3DTS_PROJECTION].u.m[0][0]);
        checkGLcall("glLoadMatrixf");
    }
}

/* This should match any arrays loaded in load_vertex_data.
 * TODO: Only load / unload arrays if we have to. */
static void unload_vertex_data(const struct wined3d_gl_info *gl_info)
{
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    if (gl_info->supported[EXT_SECONDARY_COLOR])
    {
        glDisableClientState(GL_SECONDARY_COLOR_ARRAY_EXT);
    }
    if (gl_info->supported[ARB_VERTEX_BLEND])
    {
        glDisableClientState(GL_WEIGHT_ARRAY_ARB);
    }
    unload_tex_coords(gl_info);
}

static inline void unload_numbered_array(struct wined3d_context *context, int i)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;

    GL_EXTCALL(glDisableVertexAttribArrayARB(i));
    checkGLcall("glDisableVertexAttribArrayARB(reg)");

    context->numbered_array_mask &= ~(1 << i);
}

/* This should match any arrays loaded in loadNumberedArrays
 * TODO: Only load / unload arrays if we have to. */
static void unload_numbered_arrays(struct wined3d_context *context)
{
    /* disable any attribs (this is the same for both GLSL and ARB modes) */
    int i;

    for (i = 0; i < context->gl_info->limits.vertex_attribs; ++i) {
        unload_numbered_array(context, i);
    }
}

static void load_numbered_arrays(struct wined3d_context *context,
        const struct wined3d_stream_info *stream_info, const struct wined3d_state *state)
{
    struct wined3d_device *device = context->swapchain->device;
    const struct wined3d_gl_info *gl_info = context->gl_info;
    GLuint curVBO = gl_info->supported[ARB_VERTEX_BUFFER_OBJECT] ? ~0U : 0;
    int i;

    /* Default to no instancing */
    device->instancedDraw = FALSE;

    for (i = 0; i < MAX_ATTRIBS; i++)
    {
        const struct wined3d_stream_state *stream;

        if (!(stream_info->use_map & (1 << i)))
        {
            if (context->numbered_array_mask & (1 << i)) unload_numbered_array(context, i);
            continue;
        }

        stream = &state->streams[stream_info->elements[i].stream_idx];

        /* Do not load instance data. It will be specified using glTexCoord by drawprim */
        if (stream->flags & WINED3DSTREAMSOURCE_INSTANCEDATA)
        {
            if (context->numbered_array_mask & (1 << i)) unload_numbered_array(context, i);
            device->instancedDraw = TRUE;
            continue;
        }

        TRACE_(d3d_shader)("Loading array %u [VBO=%u]\n", i, stream_info->elements[i].data.buffer_object);

        if (stream_info->elements[i].stride)
        {
            if (curVBO != stream_info->elements[i].data.buffer_object)
            {
                GL_EXTCALL(glBindBufferARB(GL_ARRAY_BUFFER_ARB, stream_info->elements[i].data.buffer_object));
                checkGLcall("glBindBufferARB");
                curVBO = stream_info->elements[i].data.buffer_object;
            }
            /* Use the VBO to find out if a vertex buffer exists, not the vb
             * pointer. vb can point to a user pointer data blob. In that case
             * curVBO will be 0. If there is a vertex buffer but no vbo we
             * won't be load converted attributes anyway. */
            GL_EXTCALL(glVertexAttribPointerARB(i, stream_info->elements[i].format->gl_vtx_format,
                    stream_info->elements[i].format->gl_vtx_type,
                    stream_info->elements[i].format->gl_normalized,
                    stream_info->elements[i].stride, stream_info->elements[i].data.addr
                    + state->load_base_vertex_index * stream_info->elements[i].stride));

            if (!(context->numbered_array_mask & (1 << i)))
            {
                GL_EXTCALL(glEnableVertexAttribArrayARB(i));
                context->numbered_array_mask |= (1 << i);
            }
        }
        else
        {
            /* Stride = 0 means always the same values.
             * glVertexAttribPointerARB doesn't do that. Instead disable the
             * pointer and set up the attribute statically. But we have to
             * figure out the system memory address. */
            const BYTE *ptr = stream_info->elements[i].data.addr;
            if (stream_info->elements[i].data.buffer_object)
            {
                ptr += (ULONG_PTR)buffer_get_sysmem(stream->buffer, gl_info);
            }

            if (context->numbered_array_mask & (1 << i)) unload_numbered_array(context, i);

            switch (stream_info->elements[i].format->id)
            {
                case WINED3DFMT_R32_FLOAT:
                    GL_EXTCALL(glVertexAttrib1fvARB(i, (const GLfloat *)ptr));
                    break;
                case WINED3DFMT_R32G32_FLOAT:
                    GL_EXTCALL(glVertexAttrib2fvARB(i, (const GLfloat *)ptr));
                    break;
                case WINED3DFMT_R32G32B32_FLOAT:
                    GL_EXTCALL(glVertexAttrib3fvARB(i, (const GLfloat *)ptr));
                    break;
                case WINED3DFMT_R32G32B32A32_FLOAT:
                    GL_EXTCALL(glVertexAttrib4fvARB(i, (const GLfloat *)ptr));
                    break;

                case WINED3DFMT_R8G8B8A8_UINT:
                    GL_EXTCALL(glVertexAttrib4NubvARB(i, ptr));
                    break;
                case WINED3DFMT_B8G8R8A8_UNORM:
                    if (gl_info->supported[ARB_VERTEX_ARRAY_BGRA])
                    {
                        const DWORD *src = (const DWORD *)ptr;
                        DWORD c = *src & 0xff00ff00;
                        c |= (*src & 0xff0000) >> 16;
                        c |= (*src & 0xff) << 16;
                        GL_EXTCALL(glVertexAttrib4NubvARB(i, (GLubyte *)&c));
                        break;
                    }
                    /* else fallthrough */
                case WINED3DFMT_R8G8B8A8_UNORM:
                    GL_EXTCALL(glVertexAttrib4NubvARB(i, ptr));
                    break;

                case WINED3DFMT_R16G16_SINT:
                    GL_EXTCALL(glVertexAttrib4svARB(i, (const GLshort *)ptr));
                    break;
                case WINED3DFMT_R16G16B16A16_SINT:
                    GL_EXTCALL(glVertexAttrib4svARB(i, (const GLshort *)ptr));
                    break;

                case WINED3DFMT_R16G16_SNORM:
                {
                    const GLshort s[4] = {((const GLshort *)ptr)[0], ((const GLshort *)ptr)[1], 0, 1};
                    GL_EXTCALL(glVertexAttrib4NsvARB(i, s));
                    break;
                }
                case WINED3DFMT_R16G16_UNORM:
                {
                    const GLushort s[4] = {((const GLushort *)ptr)[0], ((const GLushort *)ptr)[1], 0, 1};
                    GL_EXTCALL(glVertexAttrib4NusvARB(i, s));
                    break;
                }
                case WINED3DFMT_R16G16B16A16_SNORM:
                    GL_EXTCALL(glVertexAttrib4NsvARB(i, (const GLshort *)ptr));
                    break;
                case WINED3DFMT_R16G16B16A16_UNORM:
                    GL_EXTCALL(glVertexAttrib4NusvARB(i, (const GLushort *)ptr));
                    break;

                case WINED3DFMT_R10G10B10A2_UINT:
                    FIXME("Unsure about WINED3DDECLTYPE_UDEC3\n");
                    /*glVertexAttrib3usvARB(i, (const GLushort *)ptr); Does not exist */
                    break;
                case WINED3DFMT_R10G10B10A2_SNORM:
                    FIXME("Unsure about WINED3DDECLTYPE_DEC3N\n");
                    /*glVertexAttrib3NusvARB(i, (const GLushort *)ptr); Does not exist */
                    break;

                case WINED3DFMT_R16G16_FLOAT:
                    /* Are those 16 bit floats. C doesn't have a 16 bit float type. I could read the single bits and calculate a 4
                     * byte float according to the IEEE standard
                     */
                    FIXME("Unsupported WINED3DDECLTYPE_FLOAT16_2\n");
                    break;
                case WINED3DFMT_R16G16B16A16_FLOAT:
                    FIXME("Unsupported WINED3DDECLTYPE_FLOAT16_4\n");
                    break;

                default:
                    ERR("Unexpected declaration in stride 0 attributes\n");
                    break;

            }
        }
    }
    checkGLcall("Loading numbered arrays");
}

static void load_vertex_data(const struct wined3d_context *context,
        const struct wined3d_stream_info *si, const struct wined3d_state *state)
{
    struct wined3d_device *device = context->swapchain->device;
    const struct wined3d_gl_info *gl_info = context->gl_info;
    GLuint curVBO = gl_info->supported[ARB_VERTEX_BUFFER_OBJECT] ? ~0U : 0;
    const struct wined3d_stream_info_element *e;

    TRACE("Using fast vertex array code\n");

    /* This is fixed function pipeline only, and the fixed function pipeline doesn't do instancing */
    device->instancedDraw = FALSE;

    /* Blend Data ---------------------------------------------- */
    if ((si->use_map & (1 << WINED3D_FFP_BLENDWEIGHT))
            || si->use_map & (1 << WINED3D_FFP_BLENDINDICES))
    {
        e = &si->elements[WINED3D_FFP_BLENDWEIGHT];

        if (gl_info->supported[ARB_VERTEX_BLEND])
        {
            TRACE("Blend %u %p %u\n", e->format->component_count,
                    e->data.addr + state->load_base_vertex_index * e->stride, e->stride);

            glEnableClientState(GL_WEIGHT_ARRAY_ARB);
            checkGLcall("glEnableClientState(GL_WEIGHT_ARRAY_ARB)");

            GL_EXTCALL(glVertexBlendARB(e->format->component_count + 1));

            if (curVBO != e->data.buffer_object)
            {
                GL_EXTCALL(glBindBufferARB(GL_ARRAY_BUFFER_ARB, e->data.buffer_object));
                checkGLcall("glBindBufferARB");
                curVBO = e->data.buffer_object;
            }

            TRACE("glWeightPointerARB(%#x, %#x, %#x, %p);\n",
                    e->format->gl_vtx_format,
                    e->format->gl_vtx_type,
                    e->stride,
                    e->data.addr + state->load_base_vertex_index * e->stride);
            GL_EXTCALL(glWeightPointerARB(e->format->gl_vtx_format, e->format->gl_vtx_type, e->stride,
                    e->data.addr + state->load_base_vertex_index * e->stride));

            checkGLcall("glWeightPointerARB");

            if (si->use_map & (1 << WINED3D_FFP_BLENDINDICES))
            {
                static BOOL warned;
                if (!warned)
                {
                    FIXME("blendMatrixIndices support\n");
                    warned = TRUE;
                }
            }
        } else {
            /* TODO: support blends in drawStridedSlow
             * No need to write a FIXME here, this is done after the general vertex decl decoding
             */
            WARN("unsupported blending in openGl\n");
        }
    }
    else
    {
        if (gl_info->supported[ARB_VERTEX_BLEND])
        {
            static const GLbyte one = 1;
            GL_EXTCALL(glWeightbvARB(1, &one));
            checkGLcall("glWeightbvARB(gl_info->max_blends, weights)");
        }
    }

    /* Point Size ----------------------------------------------*/
    if (si->use_map & (1 << WINED3D_FFP_PSIZE))
    {
        /* no such functionality in the fixed function GL pipeline */
        TRACE("Cannot change ptSize here in openGl\n");
        /* TODO: Implement this function in using shaders if they are available */
    }

    /* Vertex Pointers -----------------------------------------*/
    if (si->use_map & (1 << WINED3D_FFP_POSITION))
    {
        e = &si->elements[WINED3D_FFP_POSITION];

        if (curVBO != e->data.buffer_object)
        {
            GL_EXTCALL(glBindBufferARB(GL_ARRAY_BUFFER_ARB, e->data.buffer_object));
            checkGLcall("glBindBufferARB");
            curVBO = e->data.buffer_object;
        }

        /* min(WINED3D_ATR_FORMAT(position),3) to Disable RHW mode as 'w' coord
           handling for rhw mode should not impact screen position whereas in GL it does.
           This may result in very slightly distorted textures in rhw mode.
           There's always the other option of fixing the view matrix to
           prevent w from having any effect.

           This only applies to user pointer sources, in VBOs the vertices are fixed up
         */
        if (!e->data.buffer_object)
        {
            TRACE("glVertexPointer(3, %#x, %#x, %p);\n", e->format->gl_vtx_type, e->stride,
                    e->data.addr + state->load_base_vertex_index * e->stride);
            glVertexPointer(3 /* min(e->format->gl_vtx_format, 3) */, e->format->gl_vtx_type, e->stride,
                    e->data.addr + state->load_base_vertex_index * e->stride);
        }
        else
        {
            TRACE("glVertexPointer(%#x, %#x, %#x, %p);\n",
                    e->format->gl_vtx_format, e->format->gl_vtx_type, e->stride,
                    e->data.addr + state->load_base_vertex_index * e->stride);
            glVertexPointer(e->format->gl_vtx_format, e->format->gl_vtx_type, e->stride,
                    e->data.addr + state->load_base_vertex_index * e->stride);
        }
        checkGLcall("glVertexPointer(...)");
        glEnableClientState(GL_VERTEX_ARRAY);
        checkGLcall("glEnableClientState(GL_VERTEX_ARRAY)");
    }

    /* Normals -------------------------------------------------*/
    if (si->use_map & (1 << WINED3D_FFP_NORMAL))
    {
        e = &si->elements[WINED3D_FFP_NORMAL];

        if (curVBO != e->data.buffer_object)
        {
            GL_EXTCALL(glBindBufferARB(GL_ARRAY_BUFFER_ARB, e->data.buffer_object));
            checkGLcall("glBindBufferARB");
            curVBO = e->data.buffer_object;
        }

        TRACE("glNormalPointer(%#x, %#x, %p);\n", e->format->gl_vtx_type, e->stride,
                e->data.addr + state->load_base_vertex_index * e->stride);
        glNormalPointer(e->format->gl_vtx_type, e->stride,
                e->data.addr + state->load_base_vertex_index * e->stride);
        checkGLcall("glNormalPointer(...)");
        glEnableClientState(GL_NORMAL_ARRAY);
        checkGLcall("glEnableClientState(GL_NORMAL_ARRAY)");

    } else {
        glNormal3f(0, 0, 0);
        checkGLcall("glNormal3f(0, 0, 0)");
    }

    /* Diffuse Colour --------------------------------------------*/
    /*  WARNING: Data here MUST be in RGBA format, so cannot      */
    /*     go directly into fast mode from app pgm, because       */
    /*     directx requires data in BGRA format.                  */
    /* currently fixupVertices swizzles the format, but this isn't*/
    /* very practical when using VBOs                             */
    /* NOTE: Unless we write a vertex shader to swizzle the colour*/
    /* , or the user doesn't care and wants the speed advantage   */

    if (si->use_map & (1 << WINED3D_FFP_DIFFUSE))
    {
        e = &si->elements[WINED3D_FFP_DIFFUSE];

        if (curVBO != e->data.buffer_object)
        {
            GL_EXTCALL(glBindBufferARB(GL_ARRAY_BUFFER_ARB, e->data.buffer_object));
            checkGLcall("glBindBufferARB");
            curVBO = e->data.buffer_object;
        }

        TRACE("glColorPointer(%#x, %#x %#x, %p);\n",
                e->format->gl_vtx_format, e->format->gl_vtx_type, e->stride,
                e->data.addr + state->load_base_vertex_index * e->stride);
        glColorPointer(e->format->gl_vtx_format, e->format->gl_vtx_type, e->stride,
                e->data.addr + state->load_base_vertex_index * e->stride);
        checkGLcall("glColorPointer(4, GL_UNSIGNED_BYTE, ...)");
        glEnableClientState(GL_COLOR_ARRAY);
        checkGLcall("glEnableClientState(GL_COLOR_ARRAY)");

    } else {
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        checkGLcall("glColor4f(1, 1, 1, 1)");
    }

    /* Specular Colour ------------------------------------------*/
    if (si->use_map & (1 << WINED3D_FFP_SPECULAR))
    {
        TRACE("setting specular colour\n");

        e = &si->elements[WINED3D_FFP_SPECULAR];

        if (gl_info->supported[EXT_SECONDARY_COLOR])
        {
            GLenum type = e->format->gl_vtx_type;
            GLint format = e->format->gl_vtx_format;

            if (curVBO != e->data.buffer_object)
            {
                GL_EXTCALL(glBindBufferARB(GL_ARRAY_BUFFER_ARB, e->data.buffer_object));
                checkGLcall("glBindBufferARB");
                curVBO = e->data.buffer_object;
            }

            if (format != 4 || (gl_info->quirks & WINED3D_QUIRK_ALLOWS_SPECULAR_ALPHA))
            {
                /* Usually specular colors only allow 3 components, since they have no alpha. In D3D, the specular alpha
                 * contains the fog coordinate, which is passed to GL with GL_EXT_fog_coord. However, the fixed function
                 * vertex pipeline can pass the specular alpha through, and pixel shaders can read it. So it GL accepts
                 * 4 component secondary colors use it
                 */
                TRACE("glSecondaryColorPointer(%#x, %#x, %#x, %p);\n", format, type, e->stride,
                        e->data.addr + state->load_base_vertex_index * e->stride);
                GL_EXTCALL(glSecondaryColorPointerEXT(format, type, e->stride,
                        e->data.addr + state->load_base_vertex_index * e->stride));
                checkGLcall("glSecondaryColorPointerEXT(format, type, ...)");
            }
            else
            {
                switch(type)
                {
                    case GL_UNSIGNED_BYTE:
                        TRACE("glSecondaryColorPointer(3, GL_UNSIGNED_BYTE, %#x, %p);\n", e->stride,
                                e->data.addr + state->load_base_vertex_index * e->stride);
                        GL_EXTCALL(glSecondaryColorPointerEXT(3, GL_UNSIGNED_BYTE, e->stride,
                                e->data.addr + state->load_base_vertex_index * e->stride));
                        checkGLcall("glSecondaryColorPointerEXT(3, GL_UNSIGNED_BYTE, ...)");
                        break;

                    default:
                        FIXME("Add 4 component specular color pointers for type %x\n", type);
                        /* Make sure that the right color component is dropped */
                        TRACE("glSecondaryColorPointer(3, %#x, %#x, %p);\n", type, e->stride,
                                e->data.addr + state->load_base_vertex_index * e->stride);
                        GL_EXTCALL(glSecondaryColorPointerEXT(3, type, e->stride,
                                e->data.addr + state->load_base_vertex_index * e->stride));
                        checkGLcall("glSecondaryColorPointerEXT(3, type, ...)");
                }
            }
            glEnableClientState(GL_SECONDARY_COLOR_ARRAY_EXT);
            checkGLcall("glEnableClientState(GL_SECONDARY_COLOR_ARRAY_EXT)");
        }
        else
        {
            WARN("Specular colour is not supported in this GL implementation.\n");
        }
    }
    else
    {
        if (gl_info->supported[EXT_SECONDARY_COLOR])
        {
            GL_EXTCALL(glSecondaryColor3fEXT)(0, 0, 0);
            checkGLcall("glSecondaryColor3fEXT(0, 0, 0)");
        }
        else
        {
            WARN("Specular colour is not supported in this GL implementation.\n");
        }
    }

    /* Texture coords -------------------------------------------*/
    load_tex_coords(context, si, &curVBO, state);
}

static void streamsrc(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_device *device = context->swapchain->device;
    BOOL load_numbered = use_vs(state) && !device->useDrawStridedSlow;
    BOOL load_named = !use_vs(state) && !device->useDrawStridedSlow;

    if (isStateDirty(context, STATE_VDECL)) return;
    if (context->numberedArraysLoaded && !load_numbered)
    {
        unload_numbered_arrays(context);
        context->numberedArraysLoaded = FALSE;
        context->numbered_array_mask = 0;
    }
    else if (context->namedArraysLoaded)
    {
        unload_vertex_data(context->gl_info);
        context->namedArraysLoaded = FALSE;
    }

    if (load_numbered)
    {
        TRACE("Loading numbered arrays\n");
        load_numbered_arrays(context, &device->strided_streams, state);
        context->numberedArraysLoaded = TRUE;
    }
    else if (load_named)
    {
        TRACE("Loading vertex data\n");
        load_vertex_data(context, &device->strided_streams, state);
        context->namedArraysLoaded = TRUE;
    }
}

static void vdecl_miscpart(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (isStateDirty(context, STATE_STREAMSRC))
        return;
    streamsrc(context, state, STATE_STREAMSRC);
}

static void vertexdeclaration(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_device *device = context->swapchain->device;
    const struct wined3d_gl_info *gl_info = context->gl_info;
    BOOL useVertexShaderFunction = use_vs(state);
    BOOL usePixelShaderFunction = use_ps(state);
    BOOL updateFog = FALSE;
    BOOL transformed;
    BOOL wasrhw = context->last_was_rhw;
    unsigned int i;

    transformed = device->strided_streams.position_transformed;
    if (transformed != context->last_was_rhw && !useVertexShaderFunction)
        updateFog = TRUE;

    context->last_was_rhw = transformed;

    /* Don't have to apply the matrices when vertex shaders are used. When
     * vshaders are turned off this function will be called again anyway to
     * make sure they're properly set. */
    if (!useVertexShaderFunction)
    {
        /* TODO: Move this mainly to the viewport state and only apply when
         * the vp has changed or transformed / untransformed was switched. */
        if (wasrhw != context->last_was_rhw
                && !isStateDirty(context, STATE_TRANSFORM(WINED3DTS_PROJECTION))
                && !isStateDirty(context, STATE_VIEWPORT))
            transform_projection(context, state, STATE_TRANSFORM(WINED3DTS_PROJECTION));
        /* World matrix needs reapplication here only if we're switching between rhw and non-rhw
         * mode.
         *
         * If a vertex shader is used, the world matrix changed and then vertex shader unbound
         * this check will fail and the matrix not applied again. This is OK because a simple
         * world matrix change reapplies the matrix - These checks here are only to satisfy the
         * needs of the vertex declaration.
         *
         * World and view matrix go into the same gl matrix, so only apply them when neither is
         * dirty
         */
        if (transformed != wasrhw && !isStateDirty(context, STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(0)))
                && !isStateDirty(context, STATE_TRANSFORM(WINED3DTS_VIEW)))
            transform_world(context, state, STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(0)));
        if (!isStateDirty(context, STATE_RENDER(WINED3D_RS_COLORVERTEX)))
            state_colormat(context, state, STATE_RENDER(WINED3D_RS_COLORVERTEX));
        if (!isStateDirty(context, STATE_RENDER(WINED3D_RS_LIGHTING)))
            state_lighting(context, state, STATE_RENDER(WINED3D_RS_LIGHTING));

        if (context->last_was_vshader)
        {
            updateFog = TRUE;

            if (!device->vs_clipping && !isStateDirty(context, STATE_RENDER(WINED3D_RS_CLIPPLANEENABLE)))
                state_clipping(context, state, STATE_RENDER(WINED3D_RS_CLIPPLANEENABLE));

            for (i = 0; i < gl_info->limits.clipplanes; ++i)
            {
                clipplane(context, state, STATE_CLIPPLANE(i));
            }
        }
        if (!isStateDirty(context, STATE_RENDER(WINED3D_RS_NORMALIZENORMALS)))
            state_normalize(context, state, STATE_RENDER(WINED3D_RS_NORMALIZENORMALS));
    }
    else
    {
        if(!context->last_was_vshader) {
            static BOOL warned = FALSE;
            if(!device->vs_clipping) {
                /* Disable all clip planes to get defined results on all drivers. See comment in the
                 * state_clipping state handler
                 */
                for (i = 0; i < gl_info->limits.clipplanes; ++i)
                {
                    glDisable(GL_CLIP_PLANE0 + i);
                    checkGLcall("glDisable(GL_CLIP_PLANE0 + i)");
                }

                if (!warned && state->render_states[WINED3D_RS_CLIPPLANEENABLE])
                {
                    FIXME("Clipping not supported with vertex shaders\n");
                    warned = TRUE;
                }
            }
            if (wasrhw)
            {
                /* Apply the transform matrices when switching from rhw
                 * drawing to vertex shaders. Vertex shaders themselves do
                 * not need it, but the matrices are not reapplied
                 * automatically when switching back from vertex shaders to
                 * fixed function processing. So make sure we leave the fixed
                 * function vertex processing states back in a sane state
                 * before switching to shaders. */
                if (!isStateDirty(context, STATE_TRANSFORM(WINED3DTS_PROJECTION)))
                    transform_projection(context, state, STATE_TRANSFORM(WINED3DTS_PROJECTION));
                if (!isStateDirty(context, STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(0))))
                    transform_world(context, state, STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(0)));
            }
            updateFog = TRUE;

            /* Vertex shader clipping ignores the view matrix. Update all clipplanes
             * (Note: ARB shaders can read the clip planes for clipping emulation even if
             * device->vs_clipping is false.
             */
            for (i = 0; i < gl_info->limits.clipplanes; ++i)
            {
                clipplane(context, state, STATE_CLIPPLANE(i));
            }
        }
    }

    /* Vertex and pixel shaders are applied together, so let the last dirty
     * state do the application. */
    if (!isStateDirty(context, STATE_PIXELSHADER))
    {
        device->shader_backend->shader_select(context, usePixelShaderFunction, useVertexShaderFunction);

        if (!isStateDirty(context, STATE_VERTEXSHADERCONSTANT)
                && (useVertexShaderFunction || usePixelShaderFunction))
            shaderconstant(context, state, STATE_VERTEXSHADERCONSTANT);
    }

    context->last_was_vshader = useVertexShaderFunction;

    if (updateFog)
        context_apply_state(context, state, STATE_RENDER(WINED3D_RS_FOGVERTEXMODE));

    if (!useVertexShaderFunction)
    {
        unsigned int i;

        for (i = 0; i < MAX_TEXTURES; ++i)
        {
            if (!isStateDirty(context, STATE_TRANSFORM(WINED3DTS_TEXTURE0 + i)))
                transform_texture(context, state, STATE_TEXTURESTAGE(i, WINED3DTSS_TEXTURETRANSFORMFLAGS));
        }
    }
}

static void viewport_miscpart(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_surface *target = state->fb->render_targets[0];
    struct wined3d_viewport vp = state->viewport;

    if (vp.width > target->resource.width)
        vp.width = target->resource.width;
    if (vp.height > target->resource.height)
        vp.height = target->resource.height;

    glDepthRange(vp.min_z, vp.max_z);
    checkGLcall("glDepthRange");
    /* Note: GL requires lower left, DirectX supplies upper left. This is
     * reversed when using offscreen rendering. */
    if (context->render_offscreen)
    {
        glViewport(vp.x, vp.y, vp.width, vp.height);
    }
    else
    {
        UINT width, height;

        target->get_drawable_size(context, &width, &height);
        glViewport(vp.x, (height - (vp.y + vp.height)),
                vp.width, vp.height);
    }

    checkGLcall("glViewport");
}

static void viewport_vertexpart(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (!isStateDirty(context, STATE_TRANSFORM(WINED3DTS_PROJECTION)))
        transform_projection(context, state, STATE_TRANSFORM(WINED3DTS_PROJECTION));
    if (!isStateDirty(context, STATE_RENDER(WINED3D_RS_POINTSCALEENABLE)))
        state_pscale(context, state, STATE_RENDER(WINED3D_RS_POINTSCALEENABLE));
    /* Update the position fixup. */
    if (!isStateDirty(context, STATE_VERTEXSHADERCONSTANT))
        shaderconstant(context, state, STATE_VERTEXSHADERCONSTANT);
}

static void light(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    UINT Index = state_id - STATE_ACTIVELIGHT(0);
    const struct wined3d_light_info *lightInfo = state->lights[Index];

    if (!lightInfo)
    {
        glDisable(GL_LIGHT0 + Index);
        checkGLcall("glDisable(GL_LIGHT0 + Index)");
    }
    else
    {
        float quad_att;
        float colRGBA[] = {0.0f, 0.0f, 0.0f, 0.0f};

        /* Light settings are affected by the model view in OpenGL, the View transform in direct3d*/
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadMatrixf(&state->transforms[WINED3DTS_VIEW].u.m[0][0]);

        /* Diffuse: */
        colRGBA[0] = lightInfo->OriginalParms.diffuse.r;
        colRGBA[1] = lightInfo->OriginalParms.diffuse.g;
        colRGBA[2] = lightInfo->OriginalParms.diffuse.b;
        colRGBA[3] = lightInfo->OriginalParms.diffuse.a;
        glLightfv(GL_LIGHT0 + Index, GL_DIFFUSE, colRGBA);
        checkGLcall("glLightfv");

        /* Specular */
        colRGBA[0] = lightInfo->OriginalParms.specular.r;
        colRGBA[1] = lightInfo->OriginalParms.specular.g;
        colRGBA[2] = lightInfo->OriginalParms.specular.b;
        colRGBA[3] = lightInfo->OriginalParms.specular.a;
        glLightfv(GL_LIGHT0 + Index, GL_SPECULAR, colRGBA);
        checkGLcall("glLightfv");

        /* Ambient */
        colRGBA[0] = lightInfo->OriginalParms.ambient.r;
        colRGBA[1] = lightInfo->OriginalParms.ambient.g;
        colRGBA[2] = lightInfo->OriginalParms.ambient.b;
        colRGBA[3] = lightInfo->OriginalParms.ambient.a;
        glLightfv(GL_LIGHT0 + Index, GL_AMBIENT, colRGBA);
        checkGLcall("glLightfv");

        if ((lightInfo->OriginalParms.range * lightInfo->OriginalParms.range) >= FLT_MIN)
            quad_att = 1.4f / (lightInfo->OriginalParms.range * lightInfo->OriginalParms.range);
        else
            quad_att = 0.0f; /*  0 or  MAX?  (0 seems to be ok) */

        /* Do not assign attenuation values for lights that do not use them. D3D apps are free to pass any junk,
         * but gl drivers use them and may crash due to bad Attenuation values. Need for Speed most wanted sets
         * Attenuation0 to NaN and crashes in the gl lib
         */

        switch (lightInfo->OriginalParms.type)
        {
            case WINED3D_LIGHT_POINT:
                /* Position */
                glLightfv(GL_LIGHT0 + Index, GL_POSITION, &lightInfo->lightPosn[0]);
                checkGLcall("glLightfv");
                glLightf(GL_LIGHT0 + Index, GL_SPOT_CUTOFF, lightInfo->cutoff);
                checkGLcall("glLightf");
                /* Attenuation - Are these right? guessing... */
                glLightf(GL_LIGHT0 + Index, GL_CONSTANT_ATTENUATION, lightInfo->OriginalParms.attenuation0);
                checkGLcall("glLightf");
                glLightf(GL_LIGHT0 + Index, GL_LINEAR_ATTENUATION, lightInfo->OriginalParms.attenuation1);
                checkGLcall("glLightf");
                if (quad_att < lightInfo->OriginalParms.attenuation2)
                    quad_att = lightInfo->OriginalParms.attenuation2;
                glLightf(GL_LIGHT0 + Index, GL_QUADRATIC_ATTENUATION, quad_att);
                checkGLcall("glLightf");
                /* FIXME: Range */
                break;

            case WINED3D_LIGHT_SPOT:
                /* Position */
                glLightfv(GL_LIGHT0 + Index, GL_POSITION, &lightInfo->lightPosn[0]);
                checkGLcall("glLightfv");
                /* Direction */
                glLightfv(GL_LIGHT0 + Index, GL_SPOT_DIRECTION, &lightInfo->lightDirn[0]);
                checkGLcall("glLightfv");
                glLightf(GL_LIGHT0 + Index, GL_SPOT_EXPONENT, lightInfo->exponent);
                checkGLcall("glLightf");
                glLightf(GL_LIGHT0 + Index, GL_SPOT_CUTOFF, lightInfo->cutoff);
                checkGLcall("glLightf");
                /* Attenuation - Are these right? guessing... */
                glLightf(GL_LIGHT0 + Index, GL_CONSTANT_ATTENUATION, lightInfo->OriginalParms.attenuation0);
                checkGLcall("glLightf");
                glLightf(GL_LIGHT0 + Index, GL_LINEAR_ATTENUATION, lightInfo->OriginalParms.attenuation1);
                checkGLcall("glLightf");
                if (quad_att < lightInfo->OriginalParms.attenuation2)
                    quad_att = lightInfo->OriginalParms.attenuation2;
                glLightf(GL_LIGHT0 + Index, GL_QUADRATIC_ATTENUATION, quad_att);
                checkGLcall("glLightf");
                /* FIXME: Range */
                break;

            case WINED3D_LIGHT_DIRECTIONAL:
                /* Direction */
                glLightfv(GL_LIGHT0 + Index, GL_POSITION, &lightInfo->lightPosn[0]); /* Note gl uses w position of 0 for direction! */
                checkGLcall("glLightfv");
                glLightf(GL_LIGHT0 + Index, GL_SPOT_CUTOFF, lightInfo->cutoff);
                checkGLcall("glLightf");
                glLightf(GL_LIGHT0 + Index, GL_SPOT_EXPONENT, 0.0f);
                checkGLcall("glLightf");
                break;

            default:
                FIXME("Unrecognized light type %#x.\n", lightInfo->OriginalParms.type);
        }

        /* Restore the modelview matrix */
        glPopMatrix();

        glEnable(GL_LIGHT0 + Index);
        checkGLcall("glEnable(GL_LIGHT0 + Index)");
    }
}

static void scissorrect(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const RECT *r = &state->scissor_rect;

    /* Warning: glScissor uses window coordinates, not viewport coordinates,
     * so our viewport correction does not apply. Warning2: Even in windowed
     * mode the coords are relative to the window, not the screen. */
    TRACE("Setting new scissor rect to %s.\n", wine_dbgstr_rect(r));

    if (context->render_offscreen)
    {
        glScissor(r->left, r->top, r->right - r->left, r->bottom - r->top);
    }
    else
    {
        const struct wined3d_surface *target = state->fb->render_targets[0];
        UINT height;
        UINT width;

        target->get_drawable_size(context, &width, &height);
        glScissor(r->left, height - r->bottom, r->right - r->left, r->bottom - r->top);
    }
    checkGLcall("glScissor");
}

static void indexbuffer(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;

    if (state->user_stream || !state->index_buffer)
    {
        GL_EXTCALL(glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0));
    }
    else
    {
        struct wined3d_buffer *ib = state->index_buffer;
        GL_EXTCALL(glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, ib->buffer_object));
    }
}

static void frontface(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (context->render_offscreen)
    {
        glFrontFace(GL_CCW);
        checkGLcall("glFrontFace(GL_CCW)");
    } else {
        glFrontFace(GL_CW);
        checkGLcall("glFrontFace(GL_CW)");
    }
}

static void psorigin_w(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    static BOOL warned;

    if (!warned)
    {
        WARN("Point sprite coordinate origin switching not supported.\n");
        warned = TRUE;
    }
}

static void psorigin(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    GLint origin = context->render_offscreen ? GL_LOWER_LEFT : GL_UPPER_LEFT;

    if (glPointParameteri)
    {
        glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, origin);
        checkGLcall("glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, ...)");
    }
    else if (gl_info->supported[NV_POINT_SPRITE])
    {
        GL_EXTCALL(glPointParameteriNV(GL_POINT_SPRITE_COORD_ORIGIN, origin));
        checkGLcall("glPointParameteriNV(GL_POINT_SPRITE_COORD_ORIGIN, ...)");
    }
}

const struct StateEntryTemplate misc_state_template[] = {
    { STATE_RENDER(WINED3D_RS_SRCBLEND),                  { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_DESTBLEND),                 { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          state_blend         }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_EDGEANTIALIAS),             { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ANTIALIASEDLINEENABLE),     { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SEPARATEALPHABLENDENABLE),  { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SRCBLENDALPHA),             { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_DESTBLENDALPHA),            { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_DESTBLENDALPHA),            { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_BLENDOPALPHA),              { STATE_RENDER(WINED3D_RS_ALPHABLENDENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_STREAMSRC,                                    { STATE_STREAMSRC,                                    streamsrc           }, WINED3D_GL_EXT_NONE             },
    { STATE_VDECL,                                        { STATE_VDECL,                                        vdecl_miscpart      }, WINED3D_GL_EXT_NONE             },
    { STATE_FRONTFACE,                                    { STATE_FRONTFACE,                                    frontface           }, WINED3D_GL_EXT_NONE             },
    { STATE_SCISSORRECT,                                  { STATE_SCISSORRECT,                                  scissorrect         }, WINED3D_GL_EXT_NONE             },
    { STATE_POINTSPRITECOORDORIGIN,                       { STATE_POINTSPRITECOORDORIGIN,                       psorigin            }, WINED3D_GL_VERSION_2_0          },
    { STATE_POINTSPRITECOORDORIGIN,                       { STATE_POINTSPRITECOORDORIGIN,                       psorigin_w          }, WINED3D_GL_EXT_NONE             },

    /* TODO: Move shader constant loading to vertex and fragment pipeline respectively, as soon as the pshader and
     * vshader loadings are untied from each other
     */
    { STATE_VERTEXSHADERCONSTANT,                         { STATE_VERTEXSHADERCONSTANT,                         shaderconstant      }, WINED3D_GL_EXT_NONE             },
    { STATE_PIXELSHADERCONSTANT,                          { STATE_VERTEXSHADERCONSTANT,                         NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVMAT00),     { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVMAT00),     shader_bumpenvmat   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVMAT01),     { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVMAT10),     { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVMAT11),     { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVMAT00),     { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVMAT00),     shader_bumpenvmat   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVMAT01),     { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVMAT10),     { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVMAT11),     { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVMAT00),     { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVMAT00),     shader_bumpenvmat   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVMAT01),     { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVMAT10),     { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVMAT11),     { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVMAT00),     { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVMAT00),     shader_bumpenvmat   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVMAT01),     { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVMAT10),     { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVMAT11),     { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVMAT00),     { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVMAT00),     shader_bumpenvmat   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVMAT01),     { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVMAT10),     { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVMAT11),     { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVMAT00),     { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVMAT00),     shader_bumpenvmat   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVMAT01),     { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVMAT10),     { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVMAT11),     { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVMAT00),     { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVMAT00),     shader_bumpenvmat   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVMAT01),     { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVMAT10),     { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVMAT11),     { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVMAT00),     { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVMAT00),     shader_bumpenvmat   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVMAT01),     { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVMAT10),     { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVMAT11),     { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVMAT00),     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVLSCALE),    { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVLSCALE),    tex_bumpenvlscale   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVLOFFSET),   { STATE_TEXTURESTAGE(0, WINED3DTSS_BUMPENVLSCALE),    NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVLSCALE),    { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVLSCALE),    tex_bumpenvlscale   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVLOFFSET),   { STATE_TEXTURESTAGE(1, WINED3DTSS_BUMPENVLSCALE),    NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVLSCALE),    { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVLSCALE),    tex_bumpenvlscale   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVLOFFSET),   { STATE_TEXTURESTAGE(2, WINED3DTSS_BUMPENVLSCALE),    NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVLSCALE),    { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVLSCALE),    tex_bumpenvlscale   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVLOFFSET),   { STATE_TEXTURESTAGE(3, WINED3DTSS_BUMPENVLSCALE),    NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVLSCALE),    { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVLSCALE),    tex_bumpenvlscale   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVLOFFSET),   { STATE_TEXTURESTAGE(4, WINED3DTSS_BUMPENVLSCALE),    NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVLSCALE),    { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVLSCALE),    tex_bumpenvlscale   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVLOFFSET),   { STATE_TEXTURESTAGE(5, WINED3DTSS_BUMPENVLSCALE),    NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVLSCALE),    { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVLSCALE),    tex_bumpenvlscale   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVLOFFSET),   { STATE_TEXTURESTAGE(6, WINED3DTSS_BUMPENVLSCALE),    NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVLSCALE),    { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVLSCALE),    tex_bumpenvlscale   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVLOFFSET),   { STATE_TEXTURESTAGE(7, WINED3DTSS_BUMPENVLSCALE),    NULL                }, WINED3D_GL_EXT_NONE             },

    { STATE_VIEWPORT,                                     { STATE_VIEWPORT,                                     viewport_miscpart   }, WINED3D_GL_EXT_NONE             },
    { STATE_INDEXBUFFER,                                  { STATE_INDEXBUFFER,                                  indexbuffer         }, ARB_VERTEX_BUFFER_OBJECT        },
    { STATE_INDEXBUFFER,                                  { STATE_INDEXBUFFER,                                  state_nop           }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ANTIALIAS),                 { STATE_RENDER(WINED3D_RS_ANTIALIAS),                 state_antialias     }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_TEXTUREPERSPECTIVE),        { STATE_RENDER(WINED3D_RS_TEXTUREPERSPECTIVE),        state_perspective   }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ZENABLE),                   { STATE_RENDER(WINED3D_RS_ZENABLE),                   state_zenable       }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAPU),                     { STATE_RENDER(WINED3D_RS_WRAPU),                     state_wrapu         }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAPV),                     { STATE_RENDER(WINED3D_RS_WRAPV),                     state_wrapv         }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FILLMODE),                  { STATE_RENDER(WINED3D_RS_FILLMODE),                  state_fillmode      }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SHADEMODE),                 { STATE_RENDER(WINED3D_RS_SHADEMODE),                 state_shademode     }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_LINEPATTERN),               { STATE_RENDER(WINED3D_RS_LINEPATTERN),               state_linepattern   }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_MONOENABLE),                { STATE_RENDER(WINED3D_RS_MONOENABLE),                state_monoenable    }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ROP2),                      { STATE_RENDER(WINED3D_RS_ROP2),                      state_rop2          }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_PLANEMASK),                 { STATE_RENDER(WINED3D_RS_PLANEMASK),                 state_planemask     }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ZWRITEENABLE),              { STATE_RENDER(WINED3D_RS_ZWRITEENABLE),              state_zwritenable   }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ALPHATESTENABLE),           { STATE_RENDER(WINED3D_RS_ALPHATESTENABLE),           state_alpha         }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ALPHAREF),                  { STATE_RENDER(WINED3D_RS_ALPHATESTENABLE),           NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ALPHAFUNC),                 { STATE_RENDER(WINED3D_RS_ALPHATESTENABLE),           NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_COLORKEYENABLE),            { STATE_RENDER(WINED3D_RS_ALPHATESTENABLE),           NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_LASTPIXEL),                 { STATE_RENDER(WINED3D_RS_LASTPIXEL),                 state_lastpixel     }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_CULLMODE),                  { STATE_RENDER(WINED3D_RS_CULLMODE),                  state_cullmode      }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ZFUNC),                     { STATE_RENDER(WINED3D_RS_ZFUNC),                     state_zfunc         }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_DITHERENABLE),              { STATE_RENDER(WINED3D_RS_DITHERENABLE),              state_ditherenable  }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SUBPIXEL),                  { STATE_RENDER(WINED3D_RS_SUBPIXEL),                  state_subpixel      }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SUBPIXELX),                 { STATE_RENDER(WINED3D_RS_SUBPIXELX),                 state_subpixelx     }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_STIPPLEDALPHA),             { STATE_RENDER(WINED3D_RS_STIPPLEDALPHA),             state_stippledalpha }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_STIPPLEENABLE),             { STATE_RENDER(WINED3D_RS_STIPPLEENABLE),             state_stippleenable }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_MIPMAPLODBIAS),             { STATE_RENDER(WINED3D_RS_MIPMAPLODBIAS),             state_mipmaplodbias }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ANISOTROPY),                { STATE_RENDER(WINED3D_RS_ANISOTROPY),                state_anisotropy    }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FLUSHBATCH),                { STATE_RENDER(WINED3D_RS_FLUSHBATCH),                state_flushbatch    }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_TRANSLUCENTSORTINDEPENDENT),{ STATE_RENDER(WINED3D_RS_TRANSLUCENTSORTINDEPENDENT),state_translucentsi }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_STENCILENABLE),             { STATE_RENDER(WINED3D_RS_STENCILENABLE),             state_stencil       }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_STENCILFAIL),               { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_STENCILZFAIL),              { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_STENCILPASS),               { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_STENCILFUNC),               { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_STENCILREF),                { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_STENCILMASK),               { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_STENCILWRITEMASK),          { STATE_RENDER(WINED3D_RS_STENCILWRITEMASK),          state_stencilwrite2s}, EXT_STENCIL_TWO_SIDE            },
    { STATE_RENDER(WINED3D_RS_STENCILWRITEMASK),          { STATE_RENDER(WINED3D_RS_STENCILWRITEMASK),          state_stencilwrite  }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_TWOSIDEDSTENCILMODE),       { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_CCW_STENCILFAIL),           { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_CCW_STENCILZFAIL),          { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_CCW_STENCILPASS),           { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_CCW_STENCILFUNC),           { STATE_RENDER(WINED3D_RS_STENCILENABLE),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP0),                     { STATE_RENDER(WINED3D_RS_WRAP0),                     state_wrap          }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP1),                     { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP2),                     { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP3),                     { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP4),                     { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP5),                     { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP6),                     { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP7),                     { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP8),                     { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP9),                     { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP10),                    { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP11),                    { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP12),                    { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP13),                    { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP14),                    { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_WRAP15),                    { STATE_RENDER(WINED3D_RS_WRAP0),                     NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_EXTENTS),                   { STATE_RENDER(WINED3D_RS_EXTENTS),                   state_extents       }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_COLORKEYBLENDENABLE),       { STATE_RENDER(WINED3D_RS_COLORKEYBLENDENABLE),       state_ckeyblend     }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SOFTWAREVERTEXPROCESSING),  { STATE_RENDER(WINED3D_RS_SOFTWAREVERTEXPROCESSING),  state_swvp          }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_PATCHEDGESTYLE),            { STATE_RENDER(WINED3D_RS_PATCHEDGESTYLE),            state_patchedgestyle}, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_PATCHSEGMENTS),             { STATE_RENDER(WINED3D_RS_PATCHSEGMENTS),             state_patchsegments }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_POSITIONDEGREE),            { STATE_RENDER(WINED3D_RS_POSITIONDEGREE),            state_positiondegree}, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_NORMALDEGREE),              { STATE_RENDER(WINED3D_RS_NORMALDEGREE),              state_normaldegree  }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_MINTESSELLATIONLEVEL),      { STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION),NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_MAXTESSELLATIONLEVEL),      { STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION),NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ADAPTIVETESS_X),            { STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION),NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ADAPTIVETESS_Y),            { STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION),NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ADAPTIVETESS_Z),            { STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION),NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ADAPTIVETESS_W),            { STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION),NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION),{ STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION),state_nvdb          }, EXT_DEPTH_BOUNDS_TEST           },
    { STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION),{ STATE_RENDER(WINED3D_RS_ENABLEADAPTIVETESSELLATION),state_tessellation  }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_MULTISAMPLEANTIALIAS),      { STATE_RENDER(WINED3D_RS_MULTISAMPLEANTIALIAS),      state_msaa          }, ARB_MULTISAMPLE                 },
    { STATE_RENDER(WINED3D_RS_MULTISAMPLEANTIALIAS),      { STATE_RENDER(WINED3D_RS_MULTISAMPLEANTIALIAS),      state_msaa_w        }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_MULTISAMPLEMASK),           { STATE_RENDER(WINED3D_RS_MULTISAMPLEMASK),           state_multisampmask }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_DEBUGMONITORTOKEN),         { STATE_RENDER(WINED3D_RS_DEBUGMONITORTOKEN),         state_debug_monitor }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE),          { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE),          state_colorwrite0   }, EXT_DRAW_BUFFERS2               },
    { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE),          { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE),          state_colorwrite    }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_BLENDOP),                   { STATE_RENDER(WINED3D_RS_BLENDOP),                   state_blendop       }, EXT_BLEND_MINMAX                },
    { STATE_RENDER(WINED3D_RS_BLENDOP),                   { STATE_RENDER(WINED3D_RS_BLENDOP),                   state_blendop_w     }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SCISSORTESTENABLE),         { STATE_RENDER(WINED3D_RS_SCISSORTESTENABLE),         state_scissor       }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SLOPESCALEDEPTHBIAS),       { STATE_RENDER(WINED3D_RS_DEPTHBIAS),                 NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE1),         { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE1),         state_colorwrite1   }, EXT_DRAW_BUFFERS2               },
    { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE1),         { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE2),         { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE2),         state_colorwrite2   }, EXT_DRAW_BUFFERS2               },
    { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE2),         { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE3),         { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE3),         state_colorwrite3   }, EXT_DRAW_BUFFERS2               },
    { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE3),         { STATE_RENDER(WINED3D_RS_COLORWRITEENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_BLENDFACTOR),               { STATE_RENDER(WINED3D_RS_BLENDFACTOR),               state_blendfactor   }, EXT_BLEND_COLOR                 },
    { STATE_RENDER(WINED3D_RS_BLENDFACTOR),               { STATE_RENDER(WINED3D_RS_BLENDFACTOR),               state_blendfactor_w }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_DEPTHBIAS),                 { STATE_RENDER(WINED3D_RS_DEPTHBIAS),                 state_depthbias     }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_ZVISIBLE),                  { STATE_RENDER(WINED3D_RS_ZVISIBLE),                  state_zvisible      }, WINED3D_GL_EXT_NONE             },
    /* Samplers */
    { STATE_SAMPLER(0),                                   { STATE_SAMPLER(0),                                   sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(1),                                   { STATE_SAMPLER(1),                                   sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(2),                                   { STATE_SAMPLER(2),                                   sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(3),                                   { STATE_SAMPLER(3),                                   sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(4),                                   { STATE_SAMPLER(4),                                   sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(5),                                   { STATE_SAMPLER(5),                                   sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(6),                                   { STATE_SAMPLER(6),                                   sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(7),                                   { STATE_SAMPLER(7),                                   sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(8),                                   { STATE_SAMPLER(8),                                   sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(9),                                   { STATE_SAMPLER(9),                                   sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(10),                                  { STATE_SAMPLER(10),                                  sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(11),                                  { STATE_SAMPLER(11),                                  sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(12),                                  { STATE_SAMPLER(12),                                  sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(13),                                  { STATE_SAMPLER(13),                                  sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(14),                                  { STATE_SAMPLER(14),                                  sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(15),                                  { STATE_SAMPLER(15),                                  sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(16), /* Vertex sampler 0 */           { STATE_SAMPLER(16),                                  sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(17), /* Vertex sampler 1 */           { STATE_SAMPLER(17),                                  sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(18), /* Vertex sampler 2 */           { STATE_SAMPLER(18),                                  sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(19), /* Vertex sampler 3 */           { STATE_SAMPLER(19),                                  sampler             }, WINED3D_GL_EXT_NONE             },
    { STATE_BASEVERTEXINDEX,                              { STATE_BASEVERTEXINDEX,                              state_nop,          }, ARB_DRAW_ELEMENTS_BASE_VERTEX   },
    { STATE_BASEVERTEXINDEX,                              { STATE_STREAMSRC,                                    NULL,               }, WINED3D_GL_EXT_NONE             },
    { STATE_FRAMEBUFFER,                                  { STATE_FRAMEBUFFER,                                  context_state_fb    }, WINED3D_GL_EXT_NONE             },
    { STATE_PIXELSHADER,                                  { STATE_PIXELSHADER,                                  context_state_drawbuf},WINED3D_GL_EXT_NONE             },
    {0 /* Terminate */,                                   { 0,                                                  0                   }, WINED3D_GL_EXT_NONE             },
};

const struct StateEntryTemplate ffp_vertexstate_template[] = {
    { STATE_VDECL,                                        { STATE_VDECL,                                        vertexdeclaration   }, WINED3D_GL_EXT_NONE             },
    { STATE_VSHADER,                                      { STATE_VDECL,                                        NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_MATERIAL,                                     { STATE_RENDER(WINED3D_RS_SPECULARENABLE),            NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SPECULARENABLE),            { STATE_RENDER(WINED3D_RS_SPECULARENABLE),            state_specularenable}, WINED3D_GL_EXT_NONE             },
      /* Clip planes */
    { STATE_CLIPPLANE(0),                                 { STATE_CLIPPLANE(0),                                 clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(1),                                 { STATE_CLIPPLANE(1),                                 clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(2),                                 { STATE_CLIPPLANE(2),                                 clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(3),                                 { STATE_CLIPPLANE(3),                                 clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(4),                                 { STATE_CLIPPLANE(4),                                 clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(5),                                 { STATE_CLIPPLANE(5),                                 clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(6),                                 { STATE_CLIPPLANE(6),                                 clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(7),                                 { STATE_CLIPPLANE(7),                                 clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(8),                                 { STATE_CLIPPLANE(8),                                 clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(9),                                 { STATE_CLIPPLANE(9),                                 clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(10),                                { STATE_CLIPPLANE(10),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(11),                                { STATE_CLIPPLANE(11),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(12),                                { STATE_CLIPPLANE(12),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(13),                                { STATE_CLIPPLANE(13),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(14),                                { STATE_CLIPPLANE(14),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(15),                                { STATE_CLIPPLANE(15),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(16),                                { STATE_CLIPPLANE(16),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(17),                                { STATE_CLIPPLANE(17),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(18),                                { STATE_CLIPPLANE(18),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(19),                                { STATE_CLIPPLANE(19),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(20),                                { STATE_CLIPPLANE(20),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(21),                                { STATE_CLIPPLANE(21),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(22),                                { STATE_CLIPPLANE(22),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(23),                                { STATE_CLIPPLANE(23),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(24),                                { STATE_CLIPPLANE(24),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(25),                                { STATE_CLIPPLANE(25),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(26),                                { STATE_CLIPPLANE(26),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(27),                                { STATE_CLIPPLANE(27),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(28),                                { STATE_CLIPPLANE(28),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(29),                                { STATE_CLIPPLANE(29),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(30),                                { STATE_CLIPPLANE(30),                                clipplane           }, WINED3D_GL_EXT_NONE             },
    { STATE_CLIPPLANE(31),                                { STATE_CLIPPLANE(31),                                clipplane           }, WINED3D_GL_EXT_NONE             },
      /* Lights */
    { STATE_ACTIVELIGHT(0),                               { STATE_ACTIVELIGHT(0),                               light               }, WINED3D_GL_EXT_NONE             },
    { STATE_ACTIVELIGHT(1),                               { STATE_ACTIVELIGHT(1),                               light               }, WINED3D_GL_EXT_NONE             },
    { STATE_ACTIVELIGHT(2),                               { STATE_ACTIVELIGHT(2),                               light               }, WINED3D_GL_EXT_NONE             },
    { STATE_ACTIVELIGHT(3),                               { STATE_ACTIVELIGHT(3),                               light               }, WINED3D_GL_EXT_NONE             },
    { STATE_ACTIVELIGHT(4),                               { STATE_ACTIVELIGHT(4),                               light               }, WINED3D_GL_EXT_NONE             },
    { STATE_ACTIVELIGHT(5),                               { STATE_ACTIVELIGHT(5),                               light               }, WINED3D_GL_EXT_NONE             },
    { STATE_ACTIVELIGHT(6),                               { STATE_ACTIVELIGHT(6),                               light               }, WINED3D_GL_EXT_NONE             },
    { STATE_ACTIVELIGHT(7),                               { STATE_ACTIVELIGHT(7),                               light               }, WINED3D_GL_EXT_NONE             },
    /* Viewport */
    { STATE_VIEWPORT,                                     { STATE_VIEWPORT,                                     viewport_vertexpart }, WINED3D_GL_EXT_NONE             },
      /* Transform states follow                    */
    { STATE_TRANSFORM(WINED3DTS_VIEW),                    { STATE_TRANSFORM(WINED3DTS_VIEW),                    transform_view      }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_PROJECTION),              { STATE_TRANSFORM(WINED3DTS_PROJECTION),              transform_projection}, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_TEXTURE0),                { STATE_TEXTURESTAGE(0,WINED3DTSS_TEXTURETRANSFORMFLAGS), NULL            }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_TEXTURE1),                { STATE_TEXTURESTAGE(1,WINED3DTSS_TEXTURETRANSFORMFLAGS), NULL            }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_TEXTURE2),                { STATE_TEXTURESTAGE(2,WINED3DTSS_TEXTURETRANSFORMFLAGS), NULL            }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_TEXTURE3),                { STATE_TEXTURESTAGE(3,WINED3DTSS_TEXTURETRANSFORMFLAGS), NULL            }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_TEXTURE4),                { STATE_TEXTURESTAGE(4,WINED3DTSS_TEXTURETRANSFORMFLAGS), NULL            }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_TEXTURE5),                { STATE_TEXTURESTAGE(5,WINED3DTSS_TEXTURETRANSFORMFLAGS), NULL            }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_TEXTURE6),                { STATE_TEXTURESTAGE(6,WINED3DTSS_TEXTURETRANSFORMFLAGS), NULL            }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_TEXTURE7),                { STATE_TEXTURESTAGE(7,WINED3DTSS_TEXTURETRANSFORMFLAGS), NULL            }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  0)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  0)),        transform_world     }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  1)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  1)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  2)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  2)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  3)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  3)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  4)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  4)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  5)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  5)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  6)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  6)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  7)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  7)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  8)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  8)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  9)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(  9)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 10)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 10)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 11)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 11)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 12)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 12)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 13)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 13)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 14)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 14)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 15)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 15)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 16)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 16)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 17)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 17)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 18)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 18)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 19)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 19)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 20)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 20)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 21)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 21)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 22)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 22)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 23)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 23)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 24)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 24)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 25)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 25)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 26)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 26)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 27)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 27)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 28)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 28)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 29)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 29)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 30)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 30)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 31)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 31)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 32)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 32)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 33)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 33)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 34)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 34)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 35)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 35)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 36)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 36)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 37)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 37)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 38)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 38)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 39)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 39)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 40)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 40)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 41)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 41)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 42)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 42)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 43)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 43)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 44)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 44)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 45)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 45)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 46)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 46)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 47)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 47)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 48)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 48)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 49)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 49)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 50)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 50)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 51)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 51)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 52)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 52)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 53)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 53)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 54)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 54)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 55)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 55)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 56)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 56)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 57)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 57)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 58)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 58)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 59)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 59)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 60)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 60)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 61)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 61)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 62)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 62)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 63)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 63)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 64)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 64)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 65)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 65)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 66)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 66)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 67)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 67)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 68)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 68)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 69)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 69)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 70)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 70)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 71)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 71)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 72)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 72)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 73)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 73)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 74)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 74)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 75)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 75)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 76)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 76)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 77)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 77)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 78)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 78)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 79)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 79)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 80)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 80)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 81)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 81)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 82)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 82)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 83)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 83)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 84)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 84)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 85)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 85)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 86)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 86)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 87)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 87)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 88)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 88)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 89)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 89)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 90)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 90)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 91)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 91)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 92)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 92)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 93)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 93)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 94)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 94)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 95)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 95)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 96)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 96)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 97)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 97)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 98)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 98)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 99)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX( 99)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(100)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(100)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(101)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(101)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(102)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(102)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(103)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(103)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(104)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(104)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(105)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(105)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(106)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(106)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(107)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(107)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(108)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(108)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(109)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(109)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(110)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(110)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(111)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(111)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(112)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(112)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(113)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(113)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(114)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(114)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(115)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(115)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(116)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(116)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(117)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(117)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(118)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(118)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(119)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(119)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(120)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(120)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(121)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(121)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(122)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(122)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(123)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(123)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(124)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(124)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(125)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(125)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(126)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(126)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(127)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(127)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(128)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(128)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(129)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(129)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(130)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(130)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(131)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(131)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(132)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(132)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(133)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(133)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(134)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(134)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(135)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(135)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(136)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(136)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(137)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(137)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(138)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(138)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(139)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(139)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(140)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(140)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(141)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(141)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(142)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(142)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(143)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(143)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(144)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(144)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(145)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(145)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(146)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(146)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(147)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(147)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(148)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(148)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(149)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(149)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(150)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(150)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(151)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(151)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(152)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(152)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(153)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(153)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(154)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(154)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(155)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(155)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(156)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(156)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(157)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(157)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(158)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(158)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(159)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(159)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(160)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(160)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(161)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(161)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(162)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(162)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(163)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(163)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(164)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(164)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(165)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(165)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(166)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(166)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(167)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(167)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(168)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(168)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(169)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(169)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(170)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(170)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(171)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(171)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(172)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(172)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(173)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(173)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(174)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(174)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(175)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(175)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(176)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(176)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(177)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(177)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(178)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(178)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(179)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(179)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(180)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(180)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(181)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(181)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(182)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(182)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(183)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(183)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(184)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(184)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(185)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(185)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(186)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(186)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(187)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(187)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(188)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(188)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(189)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(189)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(190)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(190)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(191)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(191)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(192)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(192)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(193)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(193)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(194)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(194)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(195)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(195)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(196)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(196)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(197)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(197)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(198)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(198)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(199)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(199)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(200)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(200)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(201)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(201)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(202)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(202)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(203)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(203)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(204)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(204)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(205)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(205)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(206)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(206)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(207)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(207)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(208)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(208)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(209)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(209)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(210)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(210)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(211)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(211)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(212)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(212)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(213)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(213)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(214)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(214)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(215)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(215)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(216)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(216)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(217)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(217)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(218)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(218)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(219)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(219)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(220)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(220)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(221)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(221)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(222)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(222)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(223)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(223)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(224)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(224)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(225)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(225)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(226)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(226)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(227)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(227)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(228)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(228)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(229)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(229)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(230)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(230)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(231)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(231)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(232)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(232)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(233)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(233)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(234)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(234)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(235)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(235)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(236)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(236)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(237)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(237)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(238)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(238)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(239)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(239)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(240)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(240)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(241)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(241)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(242)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(242)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(243)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(243)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(244)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(244)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(245)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(245)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(246)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(246)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(247)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(247)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(248)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(248)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(249)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(249)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(250)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(250)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(251)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(251)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(252)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(252)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(253)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(253)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(254)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(254)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(255)),        { STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(255)),        transform_worldex   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0,WINED3DTSS_TEXTURETRANSFORMFLAGS),{STATE_TEXTURESTAGE(0,WINED3DTSS_TEXTURETRANSFORMFLAGS),transform_texture   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1,WINED3DTSS_TEXTURETRANSFORMFLAGS),{STATE_TEXTURESTAGE(1,WINED3DTSS_TEXTURETRANSFORMFLAGS),transform_texture   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2,WINED3DTSS_TEXTURETRANSFORMFLAGS),{STATE_TEXTURESTAGE(2,WINED3DTSS_TEXTURETRANSFORMFLAGS),transform_texture   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3,WINED3DTSS_TEXTURETRANSFORMFLAGS),{STATE_TEXTURESTAGE(3,WINED3DTSS_TEXTURETRANSFORMFLAGS),transform_texture   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4,WINED3DTSS_TEXTURETRANSFORMFLAGS),{STATE_TEXTURESTAGE(4,WINED3DTSS_TEXTURETRANSFORMFLAGS),transform_texture   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5,WINED3DTSS_TEXTURETRANSFORMFLAGS),{STATE_TEXTURESTAGE(5,WINED3DTSS_TEXTURETRANSFORMFLAGS),transform_texture   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6,WINED3DTSS_TEXTURETRANSFORMFLAGS),{STATE_TEXTURESTAGE(6,WINED3DTSS_TEXTURETRANSFORMFLAGS),transform_texture   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7,WINED3DTSS_TEXTURETRANSFORMFLAGS),{STATE_TEXTURESTAGE(7,WINED3DTSS_TEXTURETRANSFORMFLAGS),transform_texture   }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_TEXCOORDINDEX),    { STATE_TEXTURESTAGE(0, WINED3DTSS_TEXCOORDINDEX),    tex_coordindex      }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_TEXCOORDINDEX),    { STATE_TEXTURESTAGE(1, WINED3DTSS_TEXCOORDINDEX),    tex_coordindex      }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_TEXCOORDINDEX),    { STATE_TEXTURESTAGE(2, WINED3DTSS_TEXCOORDINDEX),    tex_coordindex      }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_TEXCOORDINDEX),    { STATE_TEXTURESTAGE(3, WINED3DTSS_TEXCOORDINDEX),    tex_coordindex      }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_TEXCOORDINDEX),    { STATE_TEXTURESTAGE(4, WINED3DTSS_TEXCOORDINDEX),    tex_coordindex      }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_TEXCOORDINDEX),    { STATE_TEXTURESTAGE(5, WINED3DTSS_TEXCOORDINDEX),    tex_coordindex      }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_TEXCOORDINDEX),    { STATE_TEXTURESTAGE(6, WINED3DTSS_TEXCOORDINDEX),    tex_coordindex      }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_TEXCOORDINDEX),    { STATE_TEXTURESTAGE(7, WINED3DTSS_TEXCOORDINDEX),    tex_coordindex      }, WINED3D_GL_EXT_NONE             },
      /* Fog */
    { STATE_RENDER(WINED3D_RS_FOGENABLE),                 { STATE_RENDER(WINED3D_RS_FOGENABLE),                 state_fog_vertexpart}, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FOGTABLEMODE),              { STATE_RENDER(WINED3D_RS_FOGENABLE),                 NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FOGVERTEXMODE),             { STATE_RENDER(WINED3D_RS_FOGENABLE),                 NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_RANGEFOGENABLE),            { STATE_RENDER(WINED3D_RS_FOGENABLE),                 NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_CLIPPING),                  { STATE_RENDER(WINED3D_RS_CLIPPING),                  state_clipping      }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_CLIPPLANEENABLE),           { STATE_RENDER(WINED3D_RS_CLIPPING),                  NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_LIGHTING),                  { STATE_RENDER(WINED3D_RS_LIGHTING),                  state_lighting      }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_AMBIENT),                   { STATE_RENDER(WINED3D_RS_AMBIENT),                   state_ambient       }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_COLORVERTEX),               { STATE_RENDER(WINED3D_RS_COLORVERTEX),               state_colormat      }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_LOCALVIEWER),               { STATE_RENDER(WINED3D_RS_LOCALVIEWER),               state_localviewer   }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_NORMALIZENORMALS),          { STATE_RENDER(WINED3D_RS_NORMALIZENORMALS),          state_normalize     }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_DIFFUSEMATERIALSOURCE),     { STATE_RENDER(WINED3D_RS_COLORVERTEX),               NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SPECULARMATERIALSOURCE),    { STATE_RENDER(WINED3D_RS_COLORVERTEX),               NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_AMBIENTMATERIALSOURCE),     { STATE_RENDER(WINED3D_RS_COLORVERTEX),               NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_EMISSIVEMATERIALSOURCE),    { STATE_RENDER(WINED3D_RS_COLORVERTEX),               NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_VERTEXBLEND),               { STATE_RENDER(WINED3D_RS_VERTEXBLEND),               state_vertexblend   }, ARB_VERTEX_BLEND                },
    { STATE_RENDER(WINED3D_RS_VERTEXBLEND),               { STATE_RENDER(WINED3D_RS_VERTEXBLEND),               state_vertexblend_w }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_POINTSIZE),                 { STATE_RENDER(WINED3D_RS_POINTSCALEENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_POINTSIZE_MIN),             { STATE_RENDER(WINED3D_RS_POINTSIZE_MIN),             state_psizemin_arb  }, ARB_POINT_PARAMETERS            },
    { STATE_RENDER(WINED3D_RS_POINTSIZE_MIN),             { STATE_RENDER(WINED3D_RS_POINTSIZE_MIN),             state_psizemin_ext  }, EXT_POINT_PARAMETERS            },
    { STATE_RENDER(WINED3D_RS_POINTSIZE_MIN),             { STATE_RENDER(WINED3D_RS_POINTSIZE_MIN),             state_psizemin_w    }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_POINTSPRITEENABLE),         { STATE_RENDER(WINED3D_RS_POINTSPRITEENABLE),         state_pointsprite   }, ARB_POINT_SPRITE                },
    { STATE_RENDER(WINED3D_RS_POINTSPRITEENABLE),         { STATE_RENDER(WINED3D_RS_POINTSPRITEENABLE),         state_pointsprite_w }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_POINTSCALEENABLE),          { STATE_RENDER(WINED3D_RS_POINTSCALEENABLE),          state_pscale        }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_POINTSCALE_A),              { STATE_RENDER(WINED3D_RS_POINTSCALEENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_POINTSCALE_B),              { STATE_RENDER(WINED3D_RS_POINTSCALEENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_POINTSCALE_C),              { STATE_RENDER(WINED3D_RS_POINTSCALEENABLE),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_POINTSIZE_MAX),             { STATE_RENDER(WINED3D_RS_POINTSIZE_MIN),             NULL                }, ARB_POINT_PARAMETERS            },
    { STATE_RENDER(WINED3D_RS_POINTSIZE_MAX),             { STATE_RENDER(WINED3D_RS_POINTSIZE_MIN),             NULL                }, EXT_POINT_PARAMETERS            },
    { STATE_RENDER(WINED3D_RS_POINTSIZE_MAX),             { STATE_RENDER(WINED3D_RS_POINTSIZE_MIN),             NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_TWEENFACTOR),               { STATE_RENDER(WINED3D_RS_VERTEXBLEND),               NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_INDEXEDVERTEXBLENDENABLE),  { STATE_RENDER(WINED3D_RS_VERTEXBLEND),               NULL                }, WINED3D_GL_EXT_NONE             },

    /* Samplers for NP2 texture matrix adjustions. They are not needed if GL_ARB_texture_non_power_of_two is supported,
     * so register a NULL state handler in that case to get the vertex part of sampler() skipped(VTF is handled in the misc states.
     * otherwise, register sampler_texmatrix, which takes care of updating the texture matrix
     */
    { STATE_SAMPLER(0),                                   { 0,                                                  NULL                }, ARB_TEXTURE_NON_POWER_OF_TWO    },
    { STATE_SAMPLER(0),                                   { 0,                                                  NULL                }, WINED3D_GL_NORMALIZED_TEXRECT   },
    { STATE_SAMPLER(0),                                   { STATE_SAMPLER(0),                                   sampler_texmatrix   }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(1),                                   { 0,                                                  NULL                }, ARB_TEXTURE_NON_POWER_OF_TWO    },
    { STATE_SAMPLER(1),                                   { 0,                                                  NULL                }, WINED3D_GL_NORMALIZED_TEXRECT   },
    { STATE_SAMPLER(1),                                   { STATE_SAMPLER(1),                                   sampler_texmatrix   }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(2),                                   { 0,                                                  NULL                }, ARB_TEXTURE_NON_POWER_OF_TWO    },
    { STATE_SAMPLER(2),                                   { 0,                                                  NULL                }, WINED3D_GL_NORMALIZED_TEXRECT   },
    { STATE_SAMPLER(2),                                   { STATE_SAMPLER(2),                                   sampler_texmatrix   }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(3),                                   { 0,                                                  NULL                }, ARB_TEXTURE_NON_POWER_OF_TWO    },
    { STATE_SAMPLER(3),                                   { 0,                                                  NULL                }, WINED3D_GL_NORMALIZED_TEXRECT   },
    { STATE_SAMPLER(3),                                   { STATE_SAMPLER(3),                                   sampler_texmatrix   }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(4),                                   { 0,                                                  NULL                }, ARB_TEXTURE_NON_POWER_OF_TWO    },
    { STATE_SAMPLER(4),                                   { 0,                                                  NULL                }, WINED3D_GL_NORMALIZED_TEXRECT   },
    { STATE_SAMPLER(4),                                   { STATE_SAMPLER(4),                                   sampler_texmatrix   }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(5),                                   { 0,                                                  NULL                }, ARB_TEXTURE_NON_POWER_OF_TWO    },
    { STATE_SAMPLER(5),                                   { 0,                                                  NULL                }, WINED3D_GL_NORMALIZED_TEXRECT   },
    { STATE_SAMPLER(5),                                   { STATE_SAMPLER(5),                                   sampler_texmatrix   }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(6),                                   { 0,                                                  NULL                }, ARB_TEXTURE_NON_POWER_OF_TWO    },
    { STATE_SAMPLER(6),                                   { 0,                                                  NULL                }, WINED3D_GL_NORMALIZED_TEXRECT   },
    { STATE_SAMPLER(6),                                   { STATE_SAMPLER(6),                                   sampler_texmatrix   }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(7),                                   { 0,                                                  NULL                }, ARB_TEXTURE_NON_POWER_OF_TWO    },
    { STATE_SAMPLER(7),                                   { 0,                                                  NULL                }, WINED3D_GL_NORMALIZED_TEXRECT   },
    { STATE_SAMPLER(7),                                   { STATE_SAMPLER(7),                                   sampler_texmatrix   }, WINED3D_GL_EXT_NONE             },
    {0 /* Terminate */,                                   { 0,                                                  0                   }, WINED3D_GL_EXT_NONE             },
};

static const struct StateEntryTemplate ffp_fragmentstate_template[] = {
    { STATE_TEXTURESTAGE(0, WINED3DTSS_COLOROP),          { STATE_TEXTURESTAGE(0, WINED3DTSS_COLOROP),          tex_colorop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_COLORARG1),        { STATE_TEXTURESTAGE(0, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_COLORARG2),        { STATE_TEXTURESTAGE(0, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_ALPHAOP),          { STATE_TEXTURESTAGE(0, WINED3DTSS_ALPHAOP),          tex_alphaop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_ALPHAARG1),        { STATE_TEXTURESTAGE(0, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_ALPHAARG2),        { STATE_TEXTURESTAGE(0, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_COLORARG0),        { STATE_TEXTURESTAGE(0, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_ALPHAARG0),        { STATE_TEXTURESTAGE(0, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_RESULTARG),        { STATE_TEXTURESTAGE(0, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(0, WINED3DTSS_CONSTANT),         { 0 /* As long as we don't support D3DTA_CONSTANT */, NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_COLOROP),          { STATE_TEXTURESTAGE(1, WINED3DTSS_COLOROP),          tex_colorop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_COLORARG1),        { STATE_TEXTURESTAGE(1, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_COLORARG2),        { STATE_TEXTURESTAGE(1, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_ALPHAOP),          { STATE_TEXTURESTAGE(1, WINED3DTSS_ALPHAOP),          tex_alphaop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_ALPHAARG1),        { STATE_TEXTURESTAGE(1, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_ALPHAARG2),        { STATE_TEXTURESTAGE(1, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_COLORARG0),        { STATE_TEXTURESTAGE(1, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_ALPHAARG0),        { STATE_TEXTURESTAGE(1, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_RESULTARG),        { STATE_TEXTURESTAGE(1, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(1, WINED3DTSS_CONSTANT),         { 0 /* As long as we don't support D3DTA_CONSTANT */, NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_COLOROP),          { STATE_TEXTURESTAGE(2, WINED3DTSS_COLOROP),          tex_colorop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_COLORARG1),        { STATE_TEXTURESTAGE(2, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_COLORARG2),        { STATE_TEXTURESTAGE(2, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_ALPHAOP),          { STATE_TEXTURESTAGE(2, WINED3DTSS_ALPHAOP),          tex_alphaop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_ALPHAARG1),        { STATE_TEXTURESTAGE(2, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_ALPHAARG2),        { STATE_TEXTURESTAGE(2, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_COLORARG0),        { STATE_TEXTURESTAGE(2, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_ALPHAARG0),        { STATE_TEXTURESTAGE(2, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_RESULTARG),        { STATE_TEXTURESTAGE(2, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(2, WINED3DTSS_CONSTANT),         { 0 /* As long as we don't support D3DTA_CONSTANT */, NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_COLOROP),          { STATE_TEXTURESTAGE(3, WINED3DTSS_COLOROP),          tex_colorop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_COLORARG1),        { STATE_TEXTURESTAGE(3, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_COLORARG2),        { STATE_TEXTURESTAGE(3, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_ALPHAOP),          { STATE_TEXTURESTAGE(3, WINED3DTSS_ALPHAOP),          tex_alphaop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_ALPHAARG1),        { STATE_TEXTURESTAGE(3, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_ALPHAARG2),        { STATE_TEXTURESTAGE(3, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_COLORARG0),        { STATE_TEXTURESTAGE(3, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_ALPHAARG0),        { STATE_TEXTURESTAGE(3, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_RESULTARG),        { STATE_TEXTURESTAGE(3, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(3, WINED3DTSS_CONSTANT),         { 0 /* As long as we don't support D3DTA_CONSTANT */, NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_COLOROP),          { STATE_TEXTURESTAGE(4, WINED3DTSS_COLOROP),          tex_colorop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_COLORARG1),        { STATE_TEXTURESTAGE(4, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_COLORARG2),        { STATE_TEXTURESTAGE(4, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_ALPHAOP),          { STATE_TEXTURESTAGE(4, WINED3DTSS_ALPHAOP),          tex_alphaop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_ALPHAARG1),        { STATE_TEXTURESTAGE(4, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_ALPHAARG2),        { STATE_TEXTURESTAGE(4, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_COLORARG0),        { STATE_TEXTURESTAGE(4, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_ALPHAARG0),        { STATE_TEXTURESTAGE(4, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_RESULTARG),        { STATE_TEXTURESTAGE(4, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(4, WINED3DTSS_CONSTANT),         { 0 /* As long as we don't support D3DTA_CONSTANT */, NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_COLOROP),          { STATE_TEXTURESTAGE(5, WINED3DTSS_COLOROP),          tex_colorop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_COLORARG1),        { STATE_TEXTURESTAGE(5, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_COLORARG2),        { STATE_TEXTURESTAGE(5, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_ALPHAOP),          { STATE_TEXTURESTAGE(5, WINED3DTSS_ALPHAOP),          tex_alphaop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_ALPHAARG1),        { STATE_TEXTURESTAGE(5, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_ALPHAARG2),        { STATE_TEXTURESTAGE(5, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_COLORARG0),        { STATE_TEXTURESTAGE(5, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_ALPHAARG0),        { STATE_TEXTURESTAGE(5, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_RESULTARG),        { STATE_TEXTURESTAGE(5, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(5, WINED3DTSS_CONSTANT),         { 0 /* As long as we don't support D3DTA_CONSTANT */, NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_COLOROP),          { STATE_TEXTURESTAGE(6, WINED3DTSS_COLOROP),          tex_colorop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_COLORARG1),        { STATE_TEXTURESTAGE(6, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_COLORARG2),        { STATE_TEXTURESTAGE(6, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_ALPHAOP),          { STATE_TEXTURESTAGE(6, WINED3DTSS_ALPHAOP),          tex_alphaop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_ALPHAARG1),        { STATE_TEXTURESTAGE(6, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_ALPHAARG2),        { STATE_TEXTURESTAGE(6, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_COLORARG0),        { STATE_TEXTURESTAGE(6, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_ALPHAARG0),        { STATE_TEXTURESTAGE(6, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_RESULTARG),        { STATE_TEXTURESTAGE(6, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(6, WINED3DTSS_CONSTANT),         { 0 /* As long as we don't support D3DTA_CONSTANT */, NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_COLOROP),          { STATE_TEXTURESTAGE(7, WINED3DTSS_COLOROP),          tex_colorop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_COLORARG1),        { STATE_TEXTURESTAGE(7, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_COLORARG2),        { STATE_TEXTURESTAGE(7, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_ALPHAOP),          { STATE_TEXTURESTAGE(7, WINED3DTSS_ALPHAOP),          tex_alphaop         }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_ALPHAARG1),        { STATE_TEXTURESTAGE(7, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_ALPHAARG2),        { STATE_TEXTURESTAGE(7, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_COLORARG0),        { STATE_TEXTURESTAGE(7, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_ALPHAARG0),        { STATE_TEXTURESTAGE(7, WINED3DTSS_ALPHAOP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_RESULTARG),        { STATE_TEXTURESTAGE(7, WINED3DTSS_COLOROP),          NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_TEXTURESTAGE(7, WINED3DTSS_CONSTANT),         { 0 /* As long as we don't support D3DTA_CONSTANT */, NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_PIXELSHADER,                                  { STATE_PIXELSHADER,                                  apply_pixelshader   }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_SRGBWRITEENABLE),           { STATE_PIXELSHADER,                                  NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_TEXTUREFACTOR),             { STATE_RENDER(WINED3D_RS_TEXTUREFACTOR),             state_texfactor     }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FOGCOLOR),                  { STATE_RENDER(WINED3D_RS_FOGCOLOR),                  state_fogcolor      }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FOGDENSITY),                { STATE_RENDER(WINED3D_RS_FOGDENSITY),                state_fogdensity    }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FOGENABLE),                 { STATE_RENDER(WINED3D_RS_FOGENABLE),                 state_fog_fragpart  }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FOGTABLEMODE),              { STATE_RENDER(WINED3D_RS_FOGENABLE),                 NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FOGVERTEXMODE),             { STATE_RENDER(WINED3D_RS_FOGENABLE),                 NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FOGSTART),                  { STATE_RENDER(WINED3D_RS_FOGSTART),                  state_fogstartend   }, WINED3D_GL_EXT_NONE             },
    { STATE_RENDER(WINED3D_RS_FOGEND),                    { STATE_RENDER(WINED3D_RS_FOGSTART),                  NULL                }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(0),                                   { STATE_SAMPLER(0),                                   sampler_texdim      }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(1),                                   { STATE_SAMPLER(1),                                   sampler_texdim      }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(2),                                   { STATE_SAMPLER(2),                                   sampler_texdim      }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(3),                                   { STATE_SAMPLER(3),                                   sampler_texdim      }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(4),                                   { STATE_SAMPLER(4),                                   sampler_texdim      }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(5),                                   { STATE_SAMPLER(5),                                   sampler_texdim      }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(6),                                   { STATE_SAMPLER(6),                                   sampler_texdim      }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(7),                                   { STATE_SAMPLER(7),                                   sampler_texdim      }, WINED3D_GL_EXT_NONE             },
    {0 /* Terminate */,                                   { 0,                                                  0                   }, WINED3D_GL_EXT_NONE             },
};

/* Context activation and GL locking are done by the caller. */
static void ffp_enable(BOOL enable) {}

static void ffp_fragment_get_caps(const struct wined3d_gl_info *gl_info, struct fragment_caps *caps)
{
    caps->PrimitiveMiscCaps = 0;
    caps->TextureOpCaps = WINED3DTEXOPCAPS_ADD
            | WINED3DTEXOPCAPS_ADDSIGNED
            | WINED3DTEXOPCAPS_ADDSIGNED2X
            | WINED3DTEXOPCAPS_MODULATE
            | WINED3DTEXOPCAPS_MODULATE2X
            | WINED3DTEXOPCAPS_MODULATE4X
            | WINED3DTEXOPCAPS_SELECTARG1
            | WINED3DTEXOPCAPS_SELECTARG2
            | WINED3DTEXOPCAPS_DISABLE;

    if (gl_info->supported[ARB_TEXTURE_ENV_COMBINE]
            || gl_info->supported[EXT_TEXTURE_ENV_COMBINE]
            || gl_info->supported[NV_TEXTURE_ENV_COMBINE4])
    {
        caps->TextureOpCaps |= WINED3DTEXOPCAPS_BLENDDIFFUSEALPHA
                | WINED3DTEXOPCAPS_BLENDTEXTUREALPHA
                | WINED3DTEXOPCAPS_BLENDFACTORALPHA
                | WINED3DTEXOPCAPS_BLENDCURRENTALPHA
                | WINED3DTEXOPCAPS_LERP
                | WINED3DTEXOPCAPS_SUBTRACT;
    }
    if (gl_info->supported[ATI_TEXTURE_ENV_COMBINE3]
            || gl_info->supported[NV_TEXTURE_ENV_COMBINE4])
    {
        caps->TextureOpCaps |= WINED3DTEXOPCAPS_ADDSMOOTH
                | WINED3DTEXOPCAPS_MULTIPLYADD
                | WINED3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR
                | WINED3DTEXOPCAPS_MODULATECOLOR_ADDALPHA
                | WINED3DTEXOPCAPS_BLENDTEXTUREALPHAPM;
    }
    if (gl_info->supported[ARB_TEXTURE_ENV_DOT3])
        caps->TextureOpCaps |= WINED3DTEXOPCAPS_DOTPRODUCT3;

    caps->MaxTextureBlendStages = gl_info->limits.textures;
    caps->MaxSimultaneousTextures = gl_info->limits.textures;
}

static HRESULT ffp_fragment_alloc(struct wined3d_device *device) { return WINED3D_OK; }
static void ffp_fragment_free(struct wined3d_device *device) {}
static BOOL ffp_color_fixup_supported(struct color_fixup_desc fixup)
{
    if (TRACE_ON(d3d))
    {
        TRACE("Checking support for fixup:\n");
        dump_color_fixup_desc(fixup);
    }

    /* We only support identity conversions. */
    if (is_identity_fixup(fixup))
    {
        TRACE("[OK]\n");
        return TRUE;
    }

    TRACE("[FAILED]\n");
    return FALSE;
}

const struct fragment_pipeline ffp_fragment_pipeline = {
    ffp_enable,
    ffp_fragment_get_caps,
    ffp_fragment_alloc,
    ffp_fragment_free,
    ffp_color_fixup_supported,
    ffp_fragmentstate_template,
    FALSE /* we cannot disable projected textures. The vertex pipe has to do it */
};

static unsigned int num_handlers(const APPLYSTATEFUNC *funcs)
{
    unsigned int i;
    for(i = 0; funcs[i]; i++);
    return i;
}

static void multistate_apply_2(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    context->swapchain->device->multistate_funcs[state_id][0](context, state, state_id);
    context->swapchain->device->multistate_funcs[state_id][1](context, state, state_id);
}

static void multistate_apply_3(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    context->swapchain->device->multistate_funcs[state_id][0](context, state, state_id);
    context->swapchain->device->multistate_funcs[state_id][1](context, state, state_id);
    context->swapchain->device->multistate_funcs[state_id][2](context, state, state_id);
}

static void prune_invalid_states(struct StateEntry *state_table, const struct wined3d_gl_info *gl_info)
{
    unsigned int start, last, i;

    start = STATE_TEXTURESTAGE(gl_info->limits.texture_stages, 0);
    last = STATE_TEXTURESTAGE(MAX_TEXTURES - 1, WINED3D_HIGHEST_TEXTURE_STATE);
    for (i = start; i <= last; ++i)
    {
        state_table[i].representative = 0;
        state_table[i].apply = state_undefined;
    }

    start = STATE_TRANSFORM(WINED3DTS_TEXTURE0 + gl_info->limits.texture_stages);
    last = STATE_TRANSFORM(WINED3DTS_TEXTURE0 + MAX_TEXTURES - 1);
    for (i = start; i <= last; ++i)
    {
        state_table[i].representative = 0;
        state_table[i].apply = state_undefined;
    }

    start = STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(gl_info->limits.blends));
    last = STATE_TRANSFORM(WINED3DTS_WORLDMATRIX(255));
    for (i = start; i <= last; ++i)
    {
        state_table[i].representative = 0;
        state_table[i].apply = state_undefined;
    }
}

static void validate_state_table(struct StateEntry *state_table)
{
    static const struct
    {
        DWORD first;
        DWORD last;
    }
    rs_holes[] =
    {
        {  1,   1},
        {  3,   3},
        { 17,  18},
        { 21,  21},
        { 42,  45},
        { 47,  47},
        { 61, 127},
        {149, 150},
        {169, 169},
        {177, 177},
        {196, 197},
        {  0,   0},
    };
    static const DWORD simple_states[] =
    {
        STATE_MATERIAL,
        STATE_VDECL,
        STATE_STREAMSRC,
        STATE_INDEXBUFFER,
        STATE_VERTEXSHADERCONSTANT,
        STATE_PIXELSHADERCONSTANT,
        STATE_VSHADER,
        STATE_PIXELSHADER,
        STATE_VIEWPORT,
        STATE_SCISSORRECT,
        STATE_FRONTFACE,
        STATE_POINTSPRITECOORDORIGIN,
        STATE_BASEVERTEXINDEX,
        STATE_FRAMEBUFFER
    };
    unsigned int i, current;

    for (i = STATE_RENDER(1), current = 0; i <= STATE_RENDER(WINEHIGHEST_RENDER_STATE); ++i)
    {
        if (!rs_holes[current].first || i < STATE_RENDER(rs_holes[current].first))
        {
            if (!state_table[i].representative)
                ERR("State %s (%#x) should have a representative.\n", debug_d3dstate(i), i);
        }
        else if (state_table[i].representative)
            ERR("State %s (%#x) shouldn't have a representative.\n", debug_d3dstate(i), i);

        if (i == STATE_RENDER(rs_holes[current].last)) ++current;
    }

    for (i = 0; i < sizeof(simple_states) / sizeof(*simple_states); ++i)
    {
        if (!state_table[simple_states[i]].representative)
            ERR("State %s (%#x) should have a representative.\n",
                    debug_d3dstate(simple_states[i]), simple_states[i]);
    }

    for (i = 0; i < STATE_HIGHEST + 1; ++i)
    {
        DWORD rep = state_table[i].representative;
        if (rep)
        {
            if (state_table[rep].representative != rep)
            {
                ERR("State %s (%#x) has invalid representative %s (%#x).\n",
                        debug_d3dstate(i), i, debug_d3dstate(rep), rep);
                state_table[i].representative = 0;
            }

            if (rep != i)
            {
                if (state_table[i].apply)
                    ERR("State %s (%#x) has both a handler and representative.\n", debug_d3dstate(i), i);
            }
            else if (!state_table[i].apply)
            {
                ERR("Self representing state %s (%#x) has no handler.\n", debug_d3dstate(i), i);
            }
        }
    }
}

HRESULT compile_state_table(struct StateEntry *StateTable, APPLYSTATEFUNC **dev_multistate_funcs,
        const struct wined3d_gl_info *gl_info, const struct StateEntryTemplate *vertex,
        const struct fragment_pipeline *fragment, const struct StateEntryTemplate *misc)
{
    unsigned int i, type, handlers;
    APPLYSTATEFUNC multistate_funcs[STATE_HIGHEST + 1][3];
    const struct StateEntryTemplate *cur;
    BOOL set[STATE_HIGHEST + 1];

    memset(multistate_funcs, 0, sizeof(multistate_funcs));

    for(i = 0; i < STATE_HIGHEST + 1; i++) {
        StateTable[i].representative = 0;
        StateTable[i].apply = state_undefined;
    }

    for(type = 0; type < 3; type++) {
        /* This switch decides the order in which the states are applied */
        switch(type) {
            case 0: cur = misc; break;
            case 1: cur = fragment->states; break;
            case 2: cur = vertex; break;
            default: cur = NULL; /* Stupid compiler */
        }
        if(!cur) continue;

        /* GL extension filtering should not prevent multiple handlers being applied from different
         * pipeline parts
         */
        memset(set, 0, sizeof(set));

        for(i = 0; cur[i].state; i++) {
            APPLYSTATEFUNC *funcs_array;

            /* Only use the first matching state with the available extension from one template.
             * e.g.
             * {D3DRS_FOOBAR, {D3DRS_FOOBAR, func1}, XYZ_FANCY},
             * {D3DRS_FOOBAR, {D3DRS_FOOBAR, func2}, 0        }
             *
             * if GL_XYZ_fancy is supported, ignore the 2nd line
             */
            if(set[cur[i].state]) continue;
            /* Skip state lines depending on unsupported extensions */
            if (!gl_info->supported[cur[i].extension]) continue;
            set[cur[i].state] = TRUE;
            /* In some cases having an extension means that nothing has to be
             * done for a state, e.g. if GL_ARB_texture_non_power_of_two is
             * supported, the texture coordinate fixup can be ignored. If the
             * apply function is used, mark the state set(done above) to prevent
             * applying later lines, but do not record anything in the state
             * table
             */
            if (!cur[i].content.representative) continue;

            handlers = num_handlers(multistate_funcs[cur[i].state]);
            multistate_funcs[cur[i].state][handlers] = cur[i].content.apply;
            switch(handlers) {
                case 0:
                    StateTable[cur[i].state].apply = cur[i].content.apply;
                    break;
                case 1:
                    StateTable[cur[i].state].apply = multistate_apply_2;
                    dev_multistate_funcs[cur[i].state] = HeapAlloc(GetProcessHeap(),
                                                                   0,
                                                                   sizeof(**dev_multistate_funcs) * 2);
                    if (!dev_multistate_funcs[cur[i].state]) {
                        goto out_of_mem;
                    }

                    dev_multistate_funcs[cur[i].state][0] = multistate_funcs[cur[i].state][0];
                    dev_multistate_funcs[cur[i].state][1] = multistate_funcs[cur[i].state][1];
                    break;
                case 2:
                    StateTable[cur[i].state].apply = multistate_apply_3;
                    funcs_array = HeapReAlloc(GetProcessHeap(),
                                              0,
                                              dev_multistate_funcs[cur[i].state],
                                              sizeof(**dev_multistate_funcs) * 3);
                    if (!funcs_array) {
                        goto out_of_mem;
                    }

                    dev_multistate_funcs[cur[i].state] = funcs_array;
                    dev_multistate_funcs[cur[i].state][2] = multistate_funcs[cur[i].state][2];
                    break;
                default:
                    ERR("Unexpected amount of state handlers for state %u: %u\n",
                        cur[i].state, handlers + 1);
            }

            if(StateTable[cur[i].state].representative &&
            StateTable[cur[i].state].representative != cur[i].content.representative) {
                FIXME("State %u has different representatives in different pipeline parts\n",
                    cur[i].state);
            }
            StateTable[cur[i].state].representative = cur[i].content.representative;
        }
    }

    prune_invalid_states(StateTable, gl_info);
    validate_state_table(StateTable);

    return WINED3D_OK;

out_of_mem:
    for (i = 0; i <= STATE_HIGHEST; ++i) {
        HeapFree(GetProcessHeap(), 0, dev_multistate_funcs[i]);
    }

    memset(dev_multistate_funcs, 0, (STATE_HIGHEST + 1)*sizeof(*dev_multistate_funcs));

    return E_OUTOFMEMORY;
}
