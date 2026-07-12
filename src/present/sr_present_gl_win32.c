#include "sr_present.h"

#ifdef _WIN32

#include <GL/gl.h>
#include <stddef.h>
#include <string.h>

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif

#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#define OVERLAY_WIDTH 128
#define OVERLAY_HEIGHT 64

typedef HGLRC(WINAPI *wgl_create_context_attribs_arb_proc)(HDC hdc, HGLRC share, const int *attribs);
typedef void(APIENTRY *gl_attach_shader_proc)(GLuint program, GLuint shader);
typedef void(APIENTRY *gl_bind_vertex_array_proc)(GLuint array);
typedef void(APIENTRY *gl_compile_shader_proc)(GLuint shader);
typedef GLuint(APIENTRY *gl_create_program_proc)(void);
typedef GLuint(APIENTRY *gl_create_shader_proc)(GLenum type);
typedef void(APIENTRY *gl_delete_program_proc)(GLuint program);
typedef void(APIENTRY *gl_delete_shader_proc)(GLuint shader);
typedef void(APIENTRY *gl_delete_vertex_arrays_proc)(GLsizei n, const GLuint *arrays);
typedef void(APIENTRY *gl_gen_vertex_arrays_proc)(GLsizei n, GLuint *arrays);
typedef GLint(APIENTRY *gl_get_uniform_location_proc)(GLuint program, const char *name);
typedef void(APIENTRY *gl_get_program_iv_proc)(GLuint program, GLenum pname, GLint *params);
typedef void(APIENTRY *gl_get_program_info_log_proc)(GLuint program, GLsizei buf_size, GLsizei *length, char *info_log);
typedef void(APIENTRY *gl_get_shader_iv_proc)(GLuint shader, GLenum pname, GLint *params);
typedef void(APIENTRY *gl_get_shader_info_log_proc)(GLuint shader, GLsizei buf_size, GLsizei *length, char *info_log);
typedef void(APIENTRY *gl_link_program_proc)(GLuint program);
typedef void(APIENTRY *gl_shader_source_proc)(GLuint shader, GLsizei count, const char *const *string, const GLint *length);
typedef void(APIENTRY *gl_uniform_1i_proc)(GLint location, GLint v0);
typedef void(APIENTRY *gl_use_program_proc)(GLuint program);

static wgl_create_context_attribs_arb_proc p_wglCreateContextAttribsARB;
static gl_attach_shader_proc p_glAttachShader;
static gl_bind_vertex_array_proc p_glBindVertexArray;
static gl_compile_shader_proc p_glCompileShader;
static gl_create_program_proc p_glCreateProgram;
static gl_create_shader_proc p_glCreateShader;
static gl_delete_program_proc p_glDeleteProgram;
static gl_delete_shader_proc p_glDeleteShader;
static gl_delete_vertex_arrays_proc p_glDeleteVertexArrays;
static gl_gen_vertex_arrays_proc p_glGenVertexArrays;
static gl_get_uniform_location_proc p_glGetUniformLocation;
static gl_get_program_iv_proc p_glGetProgramiv;
static gl_get_program_info_log_proc p_glGetProgramInfoLog;
static gl_get_shader_iv_proc p_glGetShaderiv;
static gl_get_shader_info_log_proc p_glGetShaderInfoLog;
static gl_link_program_proc p_glLinkProgram;
static gl_shader_source_proc p_glShaderSource;
static gl_uniform_1i_proc p_glUniform1i;
static gl_use_program_proc p_glUseProgram;

static void *load_gl_proc(const char *name)
{
    void *proc = (void *)wglGetProcAddress(name);
    if (!proc || proc == (void *)1 || proc == (void *)2 || proc == (void *)3 || proc == (void *)-1) {
        proc = (void *)GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
    }
    return proc;
}

static bool load_gl33_procs(void)
{
    p_glAttachShader = (gl_attach_shader_proc)load_gl_proc("glAttachShader");
    p_glBindVertexArray = (gl_bind_vertex_array_proc)load_gl_proc("glBindVertexArray");
    p_glCompileShader = (gl_compile_shader_proc)load_gl_proc("glCompileShader");
    p_glCreateProgram = (gl_create_program_proc)load_gl_proc("glCreateProgram");
    p_glCreateShader = (gl_create_shader_proc)load_gl_proc("glCreateShader");
    p_glDeleteProgram = (gl_delete_program_proc)load_gl_proc("glDeleteProgram");
    p_glDeleteShader = (gl_delete_shader_proc)load_gl_proc("glDeleteShader");
    p_glDeleteVertexArrays = (gl_delete_vertex_arrays_proc)load_gl_proc("glDeleteVertexArrays");
    p_glGenVertexArrays = (gl_gen_vertex_arrays_proc)load_gl_proc("glGenVertexArrays");
    p_glGetUniformLocation = (gl_get_uniform_location_proc)load_gl_proc("glGetUniformLocation");
    p_glGetProgramiv = (gl_get_program_iv_proc)load_gl_proc("glGetProgramiv");
    p_glGetProgramInfoLog = (gl_get_program_info_log_proc)load_gl_proc("glGetProgramInfoLog");
    p_glGetShaderiv = (gl_get_shader_iv_proc)load_gl_proc("glGetShaderiv");
    p_glGetShaderInfoLog = (gl_get_shader_info_log_proc)load_gl_proc("glGetShaderInfoLog");
    p_glLinkProgram = (gl_link_program_proc)load_gl_proc("glLinkProgram");
    p_glShaderSource = (gl_shader_source_proc)load_gl_proc("glShaderSource");
    p_glUniform1i = (gl_uniform_1i_proc)load_gl_proc("glUniform1i");
    p_glUseProgram = (gl_use_program_proc)load_gl_proc("glUseProgram");

    return p_glAttachShader && p_glBindVertexArray && p_glCompileShader &&
           p_glCreateProgram && p_glCreateShader && p_glDeleteProgram &&
           p_glDeleteShader && p_glDeleteVertexArrays && p_glGenVertexArrays &&
           p_glGetUniformLocation && p_glGetProgramiv && p_glGetProgramInfoLog &&
           p_glGetShaderiv && p_glGetShaderInfoLog && p_glLinkProgram &&
           p_glShaderSource && p_glUniform1i && p_glUseProgram;
}

static bool choose_window_pixel_format(HDC hdc)
{
    PIXELFORMATDESCRIPTOR pfd;
    int format;

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 0;
    pfd.cStencilBits = 0;
    pfd.iLayerType = PFD_MAIN_PLANE;

    format = ChoosePixelFormat(hdc, &pfd);
    return format != 0 && SetPixelFormat(hdc, format, &pfd) != FALSE;
}

static bool make_gl_context(sr_present *present)
{
    HDC hdc = (HDC)present->hdc;
    HGLRC legacy;
    HGLRC modern;
    const int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    if (!choose_window_pixel_format(hdc)) {
        return false;
    }

    legacy = wglCreateContext(hdc);
    if (!legacy || !wglMakeCurrent(hdc, legacy)) {
        if (legacy) {
            wglDeleteContext(legacy);
        }
        return false;
    }

    p_wglCreateContextAttribsARB =
        (wgl_create_context_attribs_arb_proc)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!p_wglCreateContextAttribsARB) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(legacy);
        return false;
    }

    modern = p_wglCreateContextAttribsARB(hdc, NULL, attribs);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);

    if (!modern || !wglMakeCurrent(hdc, modern)) {
        if (modern) {
            wglDeleteContext(modern);
        }
        return false;
    }

    present->glrc = modern;
    return load_gl33_procs();
}

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = p_glCreateShader(type);
    GLint ok = 0;

    p_glShaderSource(shader, 1, &source, NULL);
    p_glCompileShader(shader);
    p_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        p_glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint create_program(void)
{
    static const char *vertex_source =
        "#version 330 core\n"
        "out vec2 v_uv;\n"
        "void main(void) {\n"
        "    v_uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        "    gl_Position = vec4(v_uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);\n"
        "}\n";
    static const char *fragment_source =
        "#version 330 core\n"
        "in vec2 v_uv;\n"
        "layout(location = 0) out vec4 out_color;\n"
        "uniform sampler2D u_frame;\n"
        "void main(void) {\n"
        "    out_color = texture(u_frame, v_uv);\n"
        "}\n";

    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    GLuint program;
    GLint ok = 0;

    if (!vertex || !fragment) {
        if (vertex) {
            p_glDeleteShader(vertex);
        }
        if (fragment) {
            p_glDeleteShader(fragment);
        }
        return 0;
    }

    program = p_glCreateProgram();
    p_glAttachShader(program, vertex);
    p_glAttachShader(program, fragment);
    p_glLinkProgram(program);
    p_glGetProgramiv(program, GL_LINK_STATUS, &ok);
    p_glDeleteShader(fragment);
    p_glDeleteShader(vertex);

    if (!ok) {
        p_glDeleteProgram(program);
        return 0;
    }

    return program;
}

static void get_client_size(HWND hwnd, int *width, int *height)
{
    RECT rect;
    *width = 0;
    *height = 0;

    if (hwnd && GetClientRect(hwnd, &rect)) {
        *width = rect.right - rect.left;
        *height = rect.bottom - rect.top;
    }
}

bool sr_present_init(sr_present *present, HWND hwnd)
{
    GLint sampler;

    if (!present || !hwnd) {
        return false;
    }

    sr_present_shutdown(present);
    memset(present, 0, sizeof(*present));
    present->hwnd = hwnd;
    present->hdc = GetDC(hwnd);
    if (!present->hdc) {
        return false;
    }

    if (!make_gl_context(present)) {
        sr_present_shutdown(present);
        return false;
    }

    present->program = create_program();
    if (!present->program) {
        sr_present_shutdown(present);
        return false;
    }

    p_glUseProgram(present->program);
    sampler = p_glGetUniformLocation(present->program, "u_frame");
    if (sampler >= 0) {
        p_glUniform1i(sampler, 0);
    }

    p_glGenVertexArrays(1, &present->vao);
    p_glBindVertexArray(present->vao);

    glGenTextures(1, &present->texture);
    glBindTexture(GL_TEXTURE_2D, present->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glGenTextures(1, &present->overlay_texture);
    glBindTexture(GL_TEXTURE_2D, present->overlay_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, OVERLAY_WIDTH, OVERLAY_HEIGHT,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    present->ready = true;
    sr_present_clear(present);
    return true;
}

void sr_present_shutdown(sr_present *present)
{
    if (!present) {
        return;
    }

    if (present->glrc) {
        wglMakeCurrent((HDC)present->hdc, (HGLRC)present->glrc);
        if (present->texture) {
            glDeleteTextures(1, &present->texture);
        }
        if (present->overlay_texture) {
            glDeleteTextures(1, &present->overlay_texture);
        }
        if (present->vao && p_glDeleteVertexArrays) {
            p_glDeleteVertexArrays(1, &present->vao);
        }
        if (present->program && p_glDeleteProgram) {
            p_glDeleteProgram(present->program);
        }
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext((HGLRC)present->glrc);
    }

    if (present->hwnd && present->hdc) {
        ReleaseDC(present->hwnd, (HDC)present->hdc);
    }

    memset(present, 0, sizeof(*present));
}

void sr_present_clear(sr_present *present)
{
    int width;
    int height;

    if (!present || !present->ready || !wglMakeCurrent((HDC)present->hdc, (HGLRC)present->glrc)) {
        return;
    }

    get_client_size(present->hwnd, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SwapBuffers((HDC)present->hdc);
}

void sr_present_set_display_size(sr_present *present, uint32_t width, uint32_t height)
{
    if (!present) return;
    present->display_width = width;
    present->display_height = height;
}

static void glyph_rows(char c, uint8_t rows[7])
{
#define G(a,b,c,d,e,f,g) do { rows[0]=a; rows[1]=b; rows[2]=c; rows[3]=d; rows[4]=e; rows[5]=f; rows[6]=g; } while (0)
    switch (c) {
    case '0': G(14,17,19,21,25,17,14); break; case '1': G(4,12,4,4,4,4,14); break;
    case '2': G(14,17,1,2,4,8,31); break; case '3': G(30,1,1,14,1,1,30); break;
    case '4': G(2,6,10,18,31,2,2); break; case '5': G(31,16,16,30,1,1,30); break;
    case '6': G(14,16,16,30,17,17,14); break; case '7': G(31,1,2,4,8,8,8); break;
    case '8': G(14,17,17,14,17,17,14); break; case '9': G(14,17,17,15,1,1,14); break;
    case 'A': G(14,17,17,31,17,17,17); break; case 'C': G(14,17,16,16,16,17,14); break;
    case 'E': G(31,16,16,30,16,16,31); break; case 'H': G(17,17,17,31,17,17,17); break;
    case 'I': G(14,4,4,4,4,4,14); break; case 'M': G(17,27,21,21,17,17,17); break;
    case 'N': G(17,25,21,19,17,17,17); break; case 'O': G(14,17,17,17,17,17,14); break;
    case 'P': G(30,17,17,30,16,16,16); break; case 'R': G(30,17,17,30,20,18,17); break;
    case 'S': G(15,16,16,14,1,1,30); break; case 'T': G(31,4,4,4,4,4,4); break;
    case 'V': G(17,17,17,17,17,10,4); break; case '/': G(1,2,2,4,8,8,16); break;
    case '.': G(0,0,0,0,0,12,12); break; case ':': G(0,12,12,0,12,12,0); break;
    case '-': G(0,0,0,31,0,0,0); break; default: G(0,0,0,0,0,0,0); break;
    }
#undef G
}

void sr_present_set_overlay_text(sr_present *present, const char *text)
{
    static uint8_t pixels[OVERLAY_WIDTH * OVERLAY_HEIGHT * 4];
    if (!present || !present->ready || !text ||
        !wglMakeCurrent((HDC)present->hdc, (HGLRC)present->glrc)) return;
    for (uint32_t i = 0; i < OVERLAY_WIDTH * OVERLAY_HEIGHT; i++) {
        pixels[i * 4 + 0] = 0; pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 0; pixels[i * 4 + 3] = 160;
    }
    uint32_t pen_x = 3, pen_y = 3;
    for (const char *p = text; *p && pen_y + 7u < OVERLAY_HEIGHT; p++) {
        if (*p == '\n') { pen_x = 3; pen_y += 8; continue; }
        if (pen_x + 5u >= OVERLAY_WIDTH) continue;
        uint8_t rows[7]; glyph_rows(*p, rows);
        for (uint32_t y = 0; y < 7; y++) for (uint32_t x = 0; x < 5; x++) {
            if (rows[y] & (1u << (4u - x))) {
                const uint32_t i = ((pen_y + y) * OVERLAY_WIDTH + pen_x + x) * 4u;
                pixels[i+0] = 255; pixels[i+1] = 255; pixels[i+2] = 255; pixels[i+3] = 255;
            }
        }
        pen_x += 6;
    }
    glBindTexture(GL_TEXTURE_2D, present->overlay_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    present->has_overlay = true;
}

void sr_present_draw(sr_present *present)
{
    int width;
    int height;

    if (!present || !present->ready || !wglMakeCurrent((HDC)present->hdc, (HGLRC)present->glrc)) {
        return;
    }

    get_client_size(present->hwnd, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (present->has_frame) {
        /* VI dimensions describe pixels, not a request to stretch them to
         * the host window. Keep the image's aspect ratio and letterbox. */
        int viewport_width = width;
        int viewport_height = height;
        if (present->frame_width && present->frame_height && width > 0 && height > 0) {
            const double window_aspect = (double)width / (double)height;
            const uint32_t aspect_width = present->display_width ?
                                          present->display_width : present->frame_width;
            const uint32_t aspect_height = present->display_height ?
                                           present->display_height : present->frame_height;
            const double frame_aspect = (double)aspect_width / (double)aspect_height;
            if (window_aspect > frame_aspect) {
                viewport_width = (int)((double)height * frame_aspect);
            } else {
                viewport_height = (int)((double)width / frame_aspect);
            }
        }
        glViewport((width - viewport_width) / 2, (height - viewport_height) / 2,
                   viewport_width, viewport_height);
        p_glUseProgram(present->program);
        p_glBindVertexArray(present->vao);
        glBindTexture(GL_TEXTURE_2D, present->texture);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (present->has_overlay) {
        const int overlay_width = OVERLAY_WIDTH * 3 / 2;
        const int overlay_height = OVERLAY_HEIGHT * 3 / 2;
        glViewport(8, height - overlay_height - 8,
                   overlay_width, overlay_height);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindTexture(GL_TEXTURE_2D, present->overlay_texture);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisable(GL_BLEND);
    }

    SwapBuffers((HDC)present->hdc);
}

bool sr_present_upload_rgba8(sr_present *present,
                             const sr_rgba8 *pixels,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride_pixels)
{
    if (!present || !present->ready || !pixels || width == 0 || height == 0 || stride_pixels < width) {
        return false;
    }
    if (!wglMakeCurrent((HDC)present->hdc, (HGLRC)present->glrc)) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, present->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)stride_pixels);

    if (present->frame_width != width || present->frame_height != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        present->frame_width = width;
        present->frame_height = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    present->has_frame = true;
    sr_present_draw(present);
    return true;
}

#else

bool sr_present_init(sr_present *present, HWND hwnd)
{
    (void)present;
    (void)hwnd;
    return false;
}

void sr_present_shutdown(sr_present *present)
{
    (void)present;
}

void sr_present_clear(sr_present *present)
{
    (void)present;
}

void sr_present_set_display_size(sr_present *present, uint32_t width, uint32_t height)
{
    (void)present;
    (void)width;
    (void)height;
}

void sr_present_set_overlay_text(sr_present *present, const char *text)
{
    (void)present;
    (void)text;
}

void sr_present_draw(sr_present *present)
{
    (void)present;
}

bool sr_present_upload_rgba8(sr_present *present,
                             const sr_rgba8 *pixels,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride_pixels)
{
    (void)present;
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride_pixels;
    return false;
}

#endif
