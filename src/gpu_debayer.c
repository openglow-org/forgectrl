/*
 * gpu_debayer.c - Bayer demosaic on the i.MX6 GC880 GPU (etnaviv/Mesa)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Two fragment-shader passes turn the raw BGGR frame into the NV12
 * layout (a luma plane, then one plane of interleaved Cb/Cr) with the
 * same superpixel semantics as debayer_bggr_half_yuv420: every 2x2 Bayer
 * quad becomes one pixel (R and B taken, the two greens averaged), luma
 * per pixel, chroma per 2x2 pixel block, JFIF full-range ITU-R 601.
 *
 * Both ends are dmabufs: the V4L2 capture buffers are imported as
 * textures and the destination (the IPU stride-fix buffer, ipu_copy.h,
 * whose stride sits on the GPU's 64-byte render boundary) as the render
 * target, so the frame never crosses the CPU. Buffers are byte streams
 * to the GPU, not pictures: the raw side binds as ARGB8888 with two raw
 * rows to a texture row (four Bayer bytes, two superpixels' worth of one
 * raw row, per texel; the stack refuses a linear import one row of a
 * 5 MP frame wide, 648 texels, and takes one two rows wide, 1296), and
 * the NV12 planes as ARGB8888 too (memory bytes
 * 0..3 read and write as .b .g .r .a; it must be ARGB, not XRGB, because
 * the render engine writes an XRGB surface's X byte as opaque, clobbering
 * every fourth output byte), and the shaders do the byte indexing.
 *
 * The render is bound by texture fetches, so the layout is chosen for
 * them: a four-byte raw texel feeds two superpixels, and the Cb and Cr
 * of a chroma site come from the same fetches in one pass. Four luma
 * bytes take four fetches and two chroma sites take four, and every
 * fetch coordinate is a varying the vertex stage computes (a linear
 * function of the output position), so no fetch depends on arithmetic
 * in the fragment stage and the mirror is compiled in.
 *
 * GLES2 has no integer textures and no multiple render targets on this
 * class of GPU, which is why the passes are two (Y, CbCr) and the
 * indexing is float math with floor(). A raw frame taller than twice
 * the GPU's maximum texture size is imported as row tiles of one dmabuf
 * (the EGL import offset selects the tile) and each pass draws once per
 * tile under a scissor; the conversion is quad-local, so tile seams at
 * multiple-of-4 raw rows are exact.
 *
 * A render's completion is a native fence file descriptor where the
 * stack offers EGL_ANDROID_native_fence_sync, so the caller can poll it
 * beside the capture queue; otherwise a plain EGL fence it waits on.
 *
 * Everything is dlopen'd (libEGL.so.1, libGLESv2.so.2) and probed, so a
 * build has no GL dependency and an image without Mesa (or a kernel
 * without etnaviv) just falls back to the NEON path at runtime.
 */
#define _GNU_SOURCE
#include "gpu_debayer.h"
#include "fflog.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------- minimal EGL/GLES2 ABI */

typedef void          *EGLDisplay;
typedef void          *EGLContext;
typedef void          *EGLConfig;
typedef void          *EGLSurface;
typedef void          *EGLImageKHR;
typedef int            EGLint;
typedef unsigned int   EGLBoolean;
typedef unsigned int   EGLenum;

typedef unsigned int   GLenum;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLboolean;
typedef float          GLfloat;
typedef char           GLchar;
typedef unsigned int   GLbitfield;

#define EGL_NO_DISPLAY              ((EGLDisplay)0)
#define EGL_NO_CONTEXT              ((EGLContext)0)
#define EGL_NO_SURFACE              ((EGLSurface)0)
#define EGL_NO_IMAGE                ((EGLImageKHR)0)
#define EGL_FALSE                   0
#define EGL_TRUE                    1
#define EGL_NONE                    0x3038
#define EGL_EXTENSIONS              0x3055
#define EGL_HEIGHT                  0x3056
#define EGL_WIDTH                   0x3057
#define EGL_RENDERABLE_TYPE         0x3040
#define EGL_OPENGL_ES2_BIT          0x0004
#define EGL_CONTEXT_CLIENT_VERSION  0x3098
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_LINUX_DMA_BUF_EXT       0x3270
#define EGL_LINUX_DRM_FOURCC_EXT    0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT   0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT  0x3274
#define EGL_SYNC_FENCE_KHR          0x30F9
#define EGL_SYNC_FLUSH_COMMANDS_BIT_KHR 0x0001
#define EGL_CONDITION_SATISFIED_KHR 0x30F6
#define EGL_TIMEOUT_EXPIRED_KHR     0x30F5
#define EGL_FOREVER_KHR             0xFFFFFFFFFFFFFFFFull
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define EGL_NO_NATIVE_FENCE_FD_ANDROID (-1)
typedef void *EGLSyncKHR;
typedef uint64_t EGLTimeKHR;

#define DRM_FORMAT_ARGB8888         0x34325241  /* 'AR24' */

#define GL_FALSE                    0
#define GL_TRIANGLES                0x0004
#define GL_UNSIGNED_BYTE            0x1401
#define GL_FLOAT                    0x1406
#define GL_RGBA                     0x1908
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_TEXTURE_2D               0x0DE1
#define GL_TEXTURE0                 0x84C0
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_NEAREST                  0x2600
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_FRAMEBUFFER              0x8D40
#define GL_RENDERBUFFER             0x8D41
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#define GL_MAX_TEXTURE_SIZE         0x0D33
#define GL_MAX_RENDERBUFFER_SIZE    0x84E8
#define GL_SCISSOR_TEST             0x0C11
#define GL_EXTENSIONS               0x1F03
#define GL_NO_ERROR                 0

struct egl_api {
    void *lib;
    EGLint      (*GetError)(void);
    void       *(*GetProcAddress)(const char *);
    const char *(*QueryString)(EGLDisplay, EGLint);
    EGLBoolean  (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean  (*Terminate)(EGLDisplay);
    EGLBoolean  (*BindAPI)(EGLenum);
    EGLBoolean  (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
                                EGLint, EGLint *);
    EGLContext  (*CreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                 const EGLint *);
    EGLBoolean  (*DestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean  (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
                               EGLContext);
    /* extension procs */
    EGLDisplay  (*GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
    EGLImageKHR (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum,
                                  void *, const EGLint *);
    EGLBoolean  (*DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    EGLSyncKHR  (*CreateSyncKHR)(EGLDisplay, EGLenum, const EGLint *);
    EGLBoolean  (*DestroySyncKHR)(EGLDisplay, EGLSyncKHR);
    EGLint      (*ClientWaitSyncKHR)(EGLDisplay, EGLSyncKHR, EGLint,
                                     EGLTimeKHR);
    EGLint      (*DupNativeFenceFDANDROID)(EGLDisplay, EGLSyncKHR);
};

struct gles_api {
    void *lib;
    GLenum  (*GetError)(void);
    const unsigned char *(*GetString)(GLenum);
    void    (*GetIntegerv)(GLenum, GLint *);
    GLuint  (*CreateShader)(GLenum);
    void    (*ShaderSource)(GLuint, GLsizei, const GLchar *const *,
                            const GLint *);
    void    (*CompileShader)(GLuint);
    void    (*GetShaderiv)(GLuint, GLenum, GLint *);
    void    (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void    (*DeleteShader)(GLuint);
    GLuint  (*CreateProgram)(void);
    void    (*AttachShader)(GLuint, GLuint);
    void    (*LinkProgram)(GLuint);
    void    (*GetProgramiv)(GLuint, GLenum, GLint *);
    void    (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void    (*UseProgram)(GLuint);
    void    (*DeleteProgram)(GLuint);
    GLint   (*GetUniformLocation)(GLuint, const GLchar *);
    GLint   (*GetAttribLocation)(GLuint, const GLchar *);
    void    (*Uniform1f)(GLint, GLfloat);
    void    (*Uniform2f)(GLint, GLfloat, GLfloat);
    void    (*Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
    void    (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void    (*Uniform1i)(GLint, GLint);
    void    (*GenTextures)(GLsizei, GLuint *);
    void    (*DeleteTextures)(GLsizei, const GLuint *);
    void    (*BindTexture)(GLenum, GLuint);
    void    (*ActiveTexture)(GLenum);
    void    (*TexParameteri)(GLenum, GLenum, GLint);
    void    (*GenFramebuffers)(GLsizei, GLuint *);
    void    (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void    (*BindFramebuffer)(GLenum, GLuint);
    void    (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    GLenum  (*CheckFramebufferStatus)(GLenum);
    void    (*GenRenderbuffers)(GLsizei, GLuint *);
    void    (*DeleteRenderbuffers)(GLsizei, const GLuint *);
    void    (*BindRenderbuffer)(GLenum, GLuint);
    void    (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void    (*Scissor)(GLint, GLint, GLsizei, GLsizei);
    void    (*Enable)(GLenum);
    void    (*Disable)(GLenum);
    void    (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean,
                                   GLsizei, const void *);
    void    (*EnableVertexAttribArray)(GLuint);
    void    (*DrawArrays)(GLenum, GLint, GLsizei);
    void    (*Finish)(void);
    void    (*Flush)(void);
    /* extension procs */
    void    (*EGLImageTargetTexture2DOES)(GLenum, void *);
    void    (*EGLImageTargetRenderbufferStorageOES)(GLenum, void *);
};

#define MAX_RAW_SLOTS   8
#define MAX_DST_SLOTS   2
#define MAX_TILES       4

struct dst {
    int         attached;
    EGLImageKHR img[2];     /* Y and CbCr plane views of the one dmabuf */
    GLuint      rb[2];
    GLuint      fbo[2];
};

struct gpu_debayer {
    struct egl_api  egl;
    struct gles_api gl;
    EGLDisplay      dpy;
    EGLContext      ctx;
    int             raw_w, raw_h;
    int             hflip;
    int             tiles;          /* row tiles the raw frame imports as */
    int             tile_h;         /* texture rows (raw row pairs) per tile,
                                     * even */
    EGLImageKHR     raw_img[MAX_RAW_SLOTS][MAX_TILES];
    GLuint          raw_tex[MAX_RAW_SLOTS][MAX_TILES];
    int             raw_attached[MAX_RAW_SLOTS];
    struct dst      dst[MAX_DST_SLOTS];
    EGLSyncKHR      fence[MAX_DST_SLOTS];
    int             fence_fd[MAX_DST_SLOTS];    /* pollable completion, or -1 */
    int             fences_ok;      /* EGL_KHR_fence_sync usable */
    int             native_ok;      /* EGL_ANDROID_native_fence_sync usable */
    GLuint          prog[2];        /* the Y and CbCr passes */
    /* attrib/uniform locations, per pass */
    GLint           a_pos[2], u_out[2], u_u[2], u_v[2];
    int             max_tex;
    int             dead;
};

/* ------------------------------------------------------------- shaders */

/* The raw frame binds as ARGB8888, texture row y holding raw rows 2y
 * (texels 0..half-1) and 2y+1 (texels half..): texel t of a raw row
 * holds bytes 4t..4t+3, read as .b .g .r .a, which are two superpixels'
 * worth of that row. An even raw row reads (B, G1) of superpixel 2t in
 * .b .g and of superpixel 2t+1 in .r .a; an odd row reads (G2, R) the
 * same way.
 *
 * Each output texel needs two raw texel columns, a and b, on an even and
 * an odd raw row: four fetches. Their coordinates are linear in the
 * output position, so the vertex stage computes them from p, the
 * position in output texels: u = u_u.x * p.x + u_u.y (column a) or
 * + u_u.z (column b), plus u_u.w for the odd raw row, and v = u_v.x *
 * p.y + u_v.y. draw_pass sets the coefficients for the pass, the mirror
 * and the tile. */
static const char VS_SRC[] =
    "attribute vec2 a_pos;\n"
    "uniform vec2 u_out;\n"
    "uniform vec4 u_u;\n"
    "uniform vec2 u_v;\n"
    "varying vec2 v_ea, v_eb, v_oa, v_ob;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  vec2 p = (a_pos + 1.0) * 0.5 * u_out;\n"
    "  float v = u_v.x * p.y + u_v.y;\n"
    "  float ua = u_u.x * p.x + u_u.y;\n"
    "  float ub = u_u.x * p.x + u_u.z;\n"
    "  v_ea = vec2(ua, v);\n"
    "  v_eb = vec2(ub, v);\n"
    "  v_oa = vec2(ua + u_u.w, v);\n"
    "  v_ob = vec2(ub + u_u.w, v);\n"
    "}\n";

#define FS_HEAD \
    "uniform sampler2D u_raw;\n"                                         \
    "varying vec2 v_ea, v_eb, v_oa, v_ob;\n"                             \
    "void main() {\n"                                                    \
    "  vec4 ea = texture2D(u_raw, v_ea);\n"                              \
    "  vec4 oa = texture2D(u_raw, v_oa);\n"                              \
    "  vec4 eb = texture2D(u_raw, v_eb);\n"                              \
    "  vec4 ob = texture2D(u_raw, v_ob);\n"

/* One output texel = 4 luma bytes = 4 superpixels: both superpixels of
 * column a, then both of column b (each pair swapped under the mirror,
 * whose columns the coordinates already swapped). Memory bytes 0..3 are
 * .b .g .r .a. */
static const char FS_Y_SRC[] =
    "PRECISION\n"
    FS_HEAD
    "  vec2 la = oa.ga * 0.299 + (ea.ga + oa.br) * 0.2935 + ea.br * 0.114;\n"
    "  vec2 lb = ob.ga * 0.299 + (eb.ga + ob.br) * 0.2935 + eb.br * 0.114;\n"
    "#if FLIP\n"
    "  gl_FragColor = vec4(lb.y, la.x, la.y, lb.x);\n"
    "#else\n"
    "  gl_FragColor = vec4(lb.x, la.y, la.x, lb.y);\n"
    "#endif\n"
    "}\n";

/* One output texel = 4 bytes of the CbCr plane = the Cb and Cr of two
 * chroma sites. Each site is point-sampled from its block's top-left
 * superpixel (in the raw frame's own order), which is the first of the
 * two in its raw texel, rather than box-averaged over the 2x2: the box
 * costs more fetches, and fetches are what bound this GPU. The CPU path
 * keeps the box filter, so the one-shot compare reports chroma
 * separately (cam.c). Raw rows 4ty and 4ty+1 (texture row 2ty) feed
 * chroma row ty. */
static const char FS_C_SRC[] =
    "PRECISION\n"
    FS_HEAD
    "  vec3 p = vec3(oa.g, (ea.g + oa.b) * 0.5, ea.b);\n"   /* R G B */
    "  vec3 q = vec3(ob.g, (eb.g + ob.b) * 0.5, eb.b);\n"
    "  vec3 cb = vec3(-0.169, -0.331, 0.500);\n"
    "  vec3 cr = vec3(0.500, -0.419, -0.081);\n"
    "  float o = 128.0 / 255.0;\n"
    /* memory order Cb(p) Cr(p) Cb(q) Cr(q) is .b .g .r .a */
    "  gl_FragColor = vec4(dot(q, cb) + o, dot(p, cr) + o,\n"
    "                      dot(p, cb) + o, dot(q, cr) + o);\n"
    "}\n";

/* ------------------------------------------------------------- loading */

static int load_egl(struct egl_api *e)
{
    e->lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!e->lib) {
        fflog(LOG_INFO, "gpu: no libEGL.so.1: %s", dlerror());
        return -1;
    }
#define E(name) \
    do { \
        *(void **)&e->name = dlsym(e->lib, "egl" #name); \
        if (!e->name) { \
            fflog(LOG_WARNING, "gpu: libEGL lacks egl" #name); \
            return -1; \
        } \
    } while (0)
    E(GetError); E(GetProcAddress); E(QueryString); E(Initialize);
    E(Terminate); E(BindAPI); E(ChooseConfig); E(CreateContext);
    E(DestroyContext); E(MakeCurrent);
#undef E
    *(void **)&e->GetPlatformDisplayEXT =
        e->GetProcAddress("eglGetPlatformDisplayEXT");
    *(void **)&e->CreateImageKHR = e->GetProcAddress("eglCreateImageKHR");
    *(void **)&e->DestroyImageKHR = e->GetProcAddress("eglDestroyImageKHR");
    *(void **)&e->CreateSyncKHR = e->GetProcAddress("eglCreateSyncKHR");
    *(void **)&e->DestroySyncKHR = e->GetProcAddress("eglDestroySyncKHR");
    *(void **)&e->ClientWaitSyncKHR =
        e->GetProcAddress("eglClientWaitSyncKHR");
    *(void **)&e->DupNativeFenceFDANDROID =
        e->GetProcAddress("eglDupNativeFenceFDANDROID");
    return 0;
}

static int load_gles(struct gles_api *g,
                     void *(*proc)(const char *))
{
    g->lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!g->lib) {
        fflog(LOG_INFO, "gpu: no libGLESv2.so.2: %s", dlerror());
        return -1;
    }
#define G(name) \
    do { \
        *(void **)&g->name = dlsym(g->lib, "gl" #name); \
        if (!g->name) { \
            fflog(LOG_WARNING, "gpu: libGLESv2 lacks gl" #name); \
            return -1; \
        } \
    } while (0)
    G(GetError); G(GetString); G(GetIntegerv); G(CreateShader);
    G(ShaderSource); G(CompileShader); G(GetShaderiv);
    G(GetShaderInfoLog); G(DeleteShader); G(CreateProgram);
    G(AttachShader); G(LinkProgram); G(GetProgramiv);
    G(GetProgramInfoLog); G(UseProgram); G(DeleteProgram);
    G(GetUniformLocation); G(GetAttribLocation); G(Uniform1f);
    G(Uniform2f); G(Uniform3f); G(Uniform4f); G(Uniform1i); G(GenTextures);
    G(DeleteTextures); G(BindTexture); G(ActiveTexture); G(TexParameteri);
    G(GenFramebuffers); G(DeleteFramebuffers); G(BindFramebuffer);
    G(FramebufferRenderbuffer); G(CheckFramebufferStatus);
    G(GenRenderbuffers); G(DeleteRenderbuffers); G(BindRenderbuffer);
    G(Viewport); G(Scissor); G(Enable); G(Disable);
    G(VertexAttribPointer); G(EnableVertexAttribArray); G(DrawArrays);
    G(Finish); G(Flush);
#undef G
    *(void **)&g->EGLImageTargetTexture2DOES =
        proc("glEGLImageTargetTexture2DOES");
    *(void **)&g->EGLImageTargetRenderbufferStorageOES =
        proc("glEGLImageTargetRenderbufferStorageOES");
    if (!g->EGLImageTargetTexture2DOES ||
        !g->EGLImageTargetRenderbufferStorageOES) {
        fflog(LOG_WARNING, "gpu: GL_OES_EGL_image procs missing");
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------- GL helpers */

static GLuint compile(gpu_debayer_t *g, GLenum kind, const char *src)
{
    GLuint sh = g->gl.CreateShader(kind);
    g->gl.ShaderSource(sh, 1, (const GLchar *const *)&src, NULL);
    g->gl.CompileShader(sh);
    GLint ok = 0;
    g->gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        g->gl.GetShaderInfoLog(sh, sizeof(log), NULL, log);
        fflog(LOG_WARNING, "gpu: shader compile failed: %.200s", log);
        g->gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Substitute the PRECISION line (with the mirror's define) and build a
 * program; highp first, since the fetch coordinates need more mantissa
 * than fp16 offers across a frame this wide, then mediump so a stack
 * that lacks fragment highp still comes up (the indexing error is
 * checked by the one-shot compare, see cam.c). */
static GLuint build_prog(gpu_debayer_t *g, const char *fs_tmpl, int flip)
{
    static const char *prec[2] = { "precision highp float;",
                                   "precision mediump float;" };
    for (int i = 0; i < 2; i++) {
        char fs[4096];
        const char *at = strstr(fs_tmpl, "PRECISION");
        snprintf(fs, sizeof(fs), "%.*s%s\n#define FLIP %d%s",
                 (int)(at - fs_tmpl), fs_tmpl, prec[i], flip ? 1 : 0,
                 at + strlen("PRECISION"));
        GLuint v = compile(g, GL_VERTEX_SHADER, VS_SRC);
        GLuint f = compile(g, GL_FRAGMENT_SHADER, fs);
        if (!v || !f) {
            if (v)
                g->gl.DeleteShader(v);
            if (f)
                g->gl.DeleteShader(f);
            continue;
        }
        GLuint p = g->gl.CreateProgram();
        g->gl.AttachShader(p, v);
        g->gl.AttachShader(p, f);
        g->gl.LinkProgram(p);
        g->gl.DeleteShader(v);
        g->gl.DeleteShader(f);
        GLint ok = 0;
        g->gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
        if (ok) {
            if (i > 0)
                fflog(LOG_INFO, "gpu: fragment highp unavailable, "
                      "using mediump");
            return p;
        }
        char log[512] = "";
        g->gl.GetProgramInfoLog(p, sizeof(log), NULL, log);
        fflog(LOG_WARNING, "gpu: program link failed: %.200s", log);
        g->gl.DeleteProgram(p);
    }
    return 0;
}

static EGLImageKHR import_bytes(gpu_debayer_t *g, int fd, size_t offset,
                                int w_texels, int h, int pitch,
                                unsigned fourcc)
{
    EGLint attrs[] = {
        EGL_WIDTH,                    w_texels,
        EGL_HEIGHT,                   h,
        EGL_LINUX_DRM_FOURCC_EXT,     (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT,    fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch,
        EGL_NONE
    };
    EGLImageKHR img = g->egl.CreateImageKHR(g->dpy, EGL_NO_CONTEXT,
                                            EGL_LINUX_DMA_BUF_EXT, NULL,
                                            attrs);
    if (img == EGL_NO_IMAGE)
        fflog(LOG_WARNING, "gpu: dmabuf import failed (fd %d, %d texels x "
              "%d, pitch %d, offset %zu, fourcc %.4s): egl 0x%x", fd,
              w_texels, h, pitch, offset, (const char *)&fourcc,
              g->egl.GetError());
    return img;
}

static int gl_ok(gpu_debayer_t *g, const char *what)
{
    GLenum e = g->gl.GetError();
    if (e == GL_NO_ERROR)
        return 1;
    fflog(LOG_WARNING, "gpu: %s: gl 0x%x", what, e);
    return 0;
}

/* ------------------------------------------------------------ open/close */

gpu_debayer_t *gpu_debayer_open(int raw_w, int raw_h, int hflip)
{
    if (raw_w % 16 || raw_h % 4) {
        fflog(LOG_WARNING, "gpu: unsupported raw geometry %dx%d",
              raw_w, raw_h);
        return NULL;
    }

    gpu_debayer_t *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    for (int i = 0; i < MAX_DST_SLOTS; i++)
        g->fence_fd[i] = -1;
    g->raw_w = raw_w;
    g->raw_h = raw_h;
    g->hflip = !!hflip;

    if (load_egl(&g->egl))
        goto fail;

    const char *cext = g->egl.QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!g->egl.GetPlatformDisplayEXT || !cext ||
        !strstr(cext, "platform_surfaceless")) {
        fflog(LOG_INFO, "gpu: no surfaceless EGL platform");
        goto fail;
    }
    g->dpy = g->egl.GetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                          NULL, NULL);
    if (g->dpy == EGL_NO_DISPLAY ||
        !g->egl.Initialize(g->dpy, NULL, NULL)) {
        fflog(LOG_INFO, "gpu: EGL display init failed (0x%x) - "
              "no usable GPU?", g->egl.GetError());
        g->dpy = EGL_NO_DISPLAY;
        goto fail;
    }

    const char *dext = g->egl.QueryString(g->dpy, EGL_EXTENSIONS);
    if (!dext || !strstr(dext, "EGL_EXT_image_dma_buf_import") ||
        !strstr(dext, "EGL_KHR_surfaceless_context") ||
        !g->egl.CreateImageKHR || !g->egl.DestroyImageKHR) {
        fflog(LOG_INFO, "gpu: required EGL extensions missing");
        goto fail;
    }
    g->fences_ok = strstr(dext, "EGL_KHR_fence_sync") &&
                   g->egl.CreateSyncKHR && g->egl.DestroySyncKHR &&
                   g->egl.ClientWaitSyncKHR;
    if (!g->fences_ok)
        fflog(LOG_INFO, "gpu: no EGL fences, renders will not overlap");
    g->native_ok = g->fences_ok &&
                   strstr(dext, "EGL_ANDROID_native_fence_sync") &&
                   g->egl.DupNativeFenceFDANDROID;

    static const EGLenum EGL_OPENGL_ES_API = 0x30A0;
    g->egl.BindAPI(EGL_OPENGL_ES_API);
    /* EGL_SURFACE_TYPE must be requested as 0 explicitly: left
     * unspecified it defaults to EGL_WINDOW_BIT, and the surfaceless
     * platform has no window configs, so the choose comes back empty. */
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    static const EGLint SURFACE_TYPE = 0x3033;
    static const EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        SURFACE_TYPE, 0,
        EGL_NONE
    };
    if (!g->egl.ChooseConfig(g->dpy, cfg_attrs, &cfg, 1, &ncfg) ||
        ncfg < 1) {
        /* No config at all: a no-config context still works for pure
         * FBO rendering where the display offers the extension. */
        if (strstr(dext, "EGL_KHR_no_config_context")) {
            cfg = NULL;
            fflog(LOG_DEBUG, "gpu: using a no-config EGL context");
        } else {
            fflog(LOG_INFO, "gpu: no GLES2 EGL config");
            goto fail;
        }
    }
    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
    };
    g->ctx = g->egl.CreateContext(g->dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g->ctx == EGL_NO_CONTEXT ||
        !g->egl.MakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                            g->ctx)) {
        fflog(LOG_INFO, "gpu: GLES2 context failed (0x%x)",
              g->egl.GetError());
        goto fail;
    }

    if (load_gles(&g->gl, g->egl.GetProcAddress))
        goto fail;
    const char *glext = (const char *)g->gl.GetString(GL_EXTENSIONS);
    if (!glext || !strstr(glext, "GL_OES_EGL_image")) {
        fflog(LOG_INFO, "gpu: GL_OES_EGL_image missing");
        goto fail;
    }

    g->gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &g->max_tex);
    GLint max_rb = 0;
    g->gl.GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
    /* The raw import is raw_w/2 texels wide (two raw rows of four Bayer
     * bytes per texel) and raw_h/2 rows tall, so only the height can
     * exceed the cap; tile by rows. Output planes are raw_w/8 texels wide
     * and half the raw height at most. */
    if (raw_w / 2 > g->max_tex || raw_h / 2 > max_rb) {
        fflog(LOG_INFO, "gpu: frame exceeds GPU limits (tex %d, rb %d)",
              g->max_tex, max_rb);
        goto fail;
    }
    g->tiles = 1;
    g->tile_h = raw_h / 2;
    while (g->tile_h > g->max_tex) {
        g->tiles *= 2;
        g->tile_h /= 2;
    }
    if (g->tiles > MAX_TILES || g->tile_h % 2) {
        fflog(LOG_INFO, "gpu: cannot tile %d rows under max texture %d",
              raw_h, g->max_tex);
        goto fail;
    }

    g->prog[0] = build_prog(g, FS_Y_SRC, g->hflip);
    g->prog[1] = build_prog(g, FS_C_SRC, g->hflip);
    if (!g->prog[0] || !g->prog[1])
        goto fail;
    for (int pass = 0; pass < 2; pass++) {
        g->a_pos[pass] = g->gl.GetAttribLocation(g->prog[pass], "a_pos");
        g->u_out[pass] = g->gl.GetUniformLocation(g->prog[pass], "u_out");
        g->u_u[pass] = g->gl.GetUniformLocation(g->prog[pass], "u_u");
        g->u_v[pass] = g->gl.GetUniformLocation(g->prog[pass], "u_v");
    }

    if (!gl_ok(g, "setup"))
        goto fail;

    fflog(LOG_INFO, "gpu: GLES2 debayer up for %dx%d (%d tile%s of %d "
          "rows, max texture %d, %s fences)", raw_w, raw_h, g->tiles,
          g->tiles > 1 ? "s" : "", 2 * g->tile_h, g->max_tex,
          g->native_ok ? "native" : g->fences_ok ? "EGL" : "no");
    return g;

fail:
    gpu_debayer_close(g);
    return NULL;
}

void gpu_debayer_close(gpu_debayer_t *g)
{
    if (!g)
        return;
    if (g->dpy != EGL_NO_DISPLAY) {
        if (g->ctx != EGL_NO_CONTEXT) {
            g->egl.MakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               g->ctx);
            for (int i = 0; i < MAX_DST_SLOTS; i++)
                if (g->fence[i]) {
                    g->egl.ClientWaitSyncKHR(g->dpy, g->fence[i],
                                             EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                             (EGLTimeKHR)5000000000LL);
                    g->egl.DestroySyncKHR(g->dpy, g->fence[i]);
                    g->fence[i] = NULL;
                }
            for (int i = 0; i < MAX_RAW_SLOTS; i++)
                for (int t = 0; t < MAX_TILES; t++) {
                    if (g->raw_tex[i][t])
                        g->gl.DeleteTextures(1, &g->raw_tex[i][t]);
                    if (g->raw_img[i][t])
                        g->egl.DestroyImageKHR(g->dpy, g->raw_img[i][t]);
                }
            for (int i = 0; i < MAX_DST_SLOTS; i++)
                for (int pl = 0; pl < 2; pl++) {
                    if (g->dst[i].fbo[pl])
                        g->gl.DeleteFramebuffers(1, &g->dst[i].fbo[pl]);
                    if (g->dst[i].rb[pl])
                        g->gl.DeleteRenderbuffers(1, &g->dst[i].rb[pl]);
                    if (g->dst[i].img[pl])
                        g->egl.DestroyImageKHR(g->dpy, g->dst[i].img[pl]);
                }
            for (int pass = 0; pass < 2; pass++)
                if (g->prog[pass])
                    g->gl.DeleteProgram(g->prog[pass]);
            g->egl.MakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT);
            g->egl.DestroyContext(g->dpy, g->ctx);
        }
        g->egl.Terminate(g->dpy);
    }
    for (int i = 0; i < MAX_DST_SLOTS; i++)
        if (g->fence_fd[i] >= 0)
            close(g->fence_fd[i]);
    if (g->gl.lib)
        dlclose(g->gl.lib);
    if (g->egl.lib)
        dlclose(g->egl.lib);
    free(g);
}

/* ------------------------------------------------------------- attach */

int gpu_debayer_attach_raw(gpu_debayer_t *g, int idx, int fd)
{
    if (g->dead || idx < 0 || idx >= MAX_RAW_SLOTS)
        return -1;
    for (int t = 0; t < g->tiles; t++) {
        EGLImageKHR img = import_bytes(g, fd,
                                       (size_t)t * g->tile_h * 2 * g->raw_w,
                                       g->raw_w / 2, g->tile_h,
                                       2 * g->raw_w, DRM_FORMAT_ARGB8888);
        if (img == EGL_NO_IMAGE)
            return -1;
        GLuint tex;
        g->gl.GenTextures(1, &tex);
        g->gl.BindTexture(GL_TEXTURE_2D, tex);
        g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GL_NEAREST);
        g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                            GL_NEAREST);
        g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            GL_CLAMP_TO_EDGE);
        g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            GL_CLAMP_TO_EDGE);
        g->gl.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
        if (!gl_ok(g, "raw texture bind")) {
            g->gl.DeleteTextures(1, &tex);
            g->egl.DestroyImageKHR(g->dpy, img);
            return -1;
        }
        g->raw_img[idx][t] = img;
        g->raw_tex[idx][t] = tex;
    }
    g->raw_attached[idx] = 1;
    return 0;
}

int gpu_debayer_attach_dst(gpu_debayer_t *g, int slot, int fd,
                           int stride, size_t uv_offset, size_t buf_len)
{
    if (g->dead || slot < 0 || slot >= MAX_DST_SLOTS)
        return -1;
    const int ow = g->raw_w / 2, oh = g->raw_h / 2;
    if (stride < ow || stride % 64) {
        fflog(LOG_WARNING, "gpu: unusable destination stride %d", stride);
        return -1;
    }
    /* Each plane is followed by room for its rows rounded up to 8: the
     * render may land whole row groups past the last one it was asked
     * for, and that must stay inside the buffer. */
    size_t y_room = (size_t)stride * (size_t)((oh + 7) & ~7);
    size_t uv_room = (size_t)stride * (size_t)((oh / 2 + 7) & ~7);
    if (uv_offset < y_room || uv_offset % 64 ||
        buf_len < uv_offset + uv_room) {
        fflog(LOG_WARNING, "gpu: destination layout unusable (stride %d, "
              "CbCr at %zu, %zu bytes)", stride, uv_offset, buf_len);
        return -1;
    }
    struct {
        size_t off;
        int    h;
    } plane[2] = {
        { 0,         oh     },
        { uv_offset, oh / 2 },
    };
    struct dst *d = &g->dst[slot];
    for (int pl = 0; pl < 2; pl++) {
        /* Both planes are ow bytes wide: ow luma samples, or ow/2 sites
         * of a Cb and a Cr. */
        d->img[pl] = import_bytes(g, fd, plane[pl].off, ow / 4,
                                  plane[pl].h, stride, DRM_FORMAT_ARGB8888);
        if (d->img[pl] == EGL_NO_IMAGE)
            return -1;
        g->gl.GenRenderbuffers(1, &d->rb[pl]);
        g->gl.BindRenderbuffer(GL_RENDERBUFFER, d->rb[pl]);
        g->gl.EGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
                                                   d->img[pl]);
        g->gl.GenFramebuffers(1, &d->fbo[pl]);
        g->gl.BindFramebuffer(GL_FRAMEBUFFER, d->fbo[pl]);
        g->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_RENDERBUFFER, d->rb[pl]);
        GLenum st = g->gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE || !gl_ok(g, "dst attach")) {
            fflog(LOG_WARNING, "gpu: plane %d framebuffer incomplete "
                  "(0x%x) - the GPU cannot render into this buffer",
                  pl, st);
            return -1;
        }
    }
    d->attached = 1;
    return 0;
}

/* ------------------------------------------------------------- convert */

static const GLfloat FS_TRI[6] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };

/* Draw one pass over every tile: `pass` selects the plane (0 Y, 1 CbCr),
 * which fixes the program, the output height and the number of texture
 * rows one output row steps over. Both planes are ow bytes (ow/4 texels)
 * wide. The coefficients follow VS_SRC: at output texel tx (p.x = tx +
 * 0.5) column a is raw texel 2tx, or half - 1 - 2tx under the mirror,
 * and column b the next one over (the previous one under the mirror);
 * at output row ty the pass reads texture row ty (Y) or 2ty (CbCr). */
static void draw_pass(gpu_debayer_t *g, int idx, struct dst *d, int pass)
{
    const int ow = g->raw_w / 2, oh = g->raw_h / 2;
    const int out_w = ow / 4;
    const int out_h = pass == 0 ? oh : oh / 2;
    const int rows_per = pass == 0 ? 1 : 2;   /* texture rows per output row */
    const GLfloat tw = (GLfloat)(g->raw_w / 2);     /* texture width */
    const GLfloat half = (GLfloat)(g->raw_w / 4);   /* texels per raw row */
    const GLfloat th = (GLfloat)g->tile_h;

    g->gl.UseProgram(g->prog[pass]);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, d->fbo[pass]);
    g->gl.Viewport(0, 0, out_w, out_h);
    g->gl.VertexAttribPointer((GLuint)g->a_pos[pass], 2, GL_FLOAT, GL_FALSE,
                              0, FS_TRI);
    g->gl.EnableVertexAttribArray((GLuint)g->a_pos[pass]);
    g->gl.Uniform2f(g->u_out[pass], (GLfloat)out_w, (GLfloat)out_h);
    if (g->hflip)
        g->gl.Uniform4f(g->u_u[pass], -2.f / tw, (half + 0.5f) / tw,
                        (half - 0.5f) / tw, half / tw);
    else
        g->gl.Uniform4f(g->u_u[pass], 2.f / tw, -0.5f / tw, 0.5f / tw,
                        half / tw);

    g->gl.Enable(GL_SCISSOR_TEST);
    for (int t = 0; t < g->tiles; t++) {
        GLfloat rowbase = (GLfloat)(t * g->tile_h);
        if (pass == 0)
            g->gl.Uniform2f(g->u_v[pass], 1.f / th, -rowbase / th);
        else
            g->gl.Uniform2f(g->u_v[pass], 2.f / th, (-0.5f - rowbase) / th);
        int y0 = t * g->tile_h / rows_per;
        g->gl.Scissor(0, y0, out_w, g->tile_h / rows_per);
        g->gl.BindTexture(GL_TEXTURE_2D, g->raw_tex[idx][t]);
        g->gl.DrawArrays(GL_TRIANGLES, 0, 3);
    }
    g->gl.Disable(GL_SCISSOR_TEST);
}

int gpu_debayer_kick(gpu_debayer_t *g, int idx, int slot)
{
    if (g->dead || idx < 0 || idx >= MAX_RAW_SLOTS ||
        !g->raw_attached[idx] || slot < 0 || slot >= MAX_DST_SLOTS ||
        !g->dst[slot].attached || g->fence[slot])
        return -1;

    /* FORGECTRL_GPU_PASSES=1 draws the luma pass alone: a bench
     * diagnostic for attributing render time, never for serving
     * pictures. */
    static int npass = -1;
    if (npass < 0) {
        const char *v = getenv("FORGECTRL_GPU_PASSES");
        npass = v ? atoi(v) : 2;
        if (npass < 1 || npass > 2)
            npass = 2;
    }
    g->gl.ActiveTexture(GL_TEXTURE0);
    for (int pass = 0; pass < npass; pass++)
        draw_pass(g, idx, &g->dst[slot], pass);
    if (g->native_ok) {
        /* The native fence's file descriptor exists once the commands
         * are flushed; a stack that then refuses it still has the
         * fence to wait on. */
        static const EGLint none[] = { EGL_NONE };
        g->fence[slot] = g->egl.CreateSyncKHR(g->dpy,
                                              EGL_SYNC_NATIVE_FENCE_ANDROID,
                                              none);
        g->gl.Flush();
        if (g->fence[slot]) {
            int fd = g->egl.DupNativeFenceFDANDROID(g->dpy, g->fence[slot]);
            g->fence_fd[slot] = fd >= 0 ? fd : -1;
        }
    } else if (g->fences_ok) {
        g->fence[slot] = g->egl.CreateSyncKHR(g->dpy, EGL_SYNC_FENCE_KHR,
                                              NULL);
        g->gl.Flush();
    }
    if (!gl_ok(g, "kick")) {
        g->dead = 1;
        return -1;
    }
    return 0;
}

int gpu_debayer_fence_fd(gpu_debayer_t *g, int slot)
{
    if (slot < 0 || slot >= MAX_DST_SLOTS)
        return -1;
    return g->fence_fd[slot];
}

int gpu_debayer_wait(gpu_debayer_t *g, int slot)
{
    if (g->dead || slot < 0 || slot >= MAX_DST_SLOTS)
        return -1;
    if (g->fence_fd[slot] >= 0) {
        close(g->fence_fd[slot]);
        g->fence_fd[slot] = -1;
    }
    if (g->fence[slot]) {
        /* A render that has not signaled in five seconds is a hung GPU:
         * the instance is dead and the caller falls back to the CPU
         * path, as after any other GPU failure. */
        EGLint r = g->egl.ClientWaitSyncKHR(g->dpy, g->fence[slot],
                                            EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                            (EGLTimeKHR)5000000000LL);
        g->egl.DestroySyncKHR(g->dpy, g->fence[slot]);
        g->fence[slot] = NULL;
        if (r == EGL_TIMEOUT_EXPIRED_KHR)
            g->dead = 1;
        if (r != EGL_CONDITION_SATISFIED_KHR) {
            fflog(LOG_WARNING, "gpu: fence wait failed (0x%x)", r);
            g->dead = 1;
            return -1;
        }
        return 0;
    }
    /* No fences: drain everything. */
    g->gl.Finish();
    if (!gl_ok(g, "wait")) {
        g->dead = 1;
        return -1;
    }
    return 0;
}
