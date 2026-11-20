/* The OpenGL 3.3 core types, constants and functions the renderer uses. gfx_gl_load fetches the functions through
   the host's loader (gfx_set_loader): on Windows, opengl32.dll exports only OpenGL 1.1.

   The function pointers have gfx-prefixed names; the macros below let the renderer call them by their usual
   names. Only the renderer's files include this header. */
#ifndef GFX_GL_H
#define GFX_GL_H

#include <stddef.h>

#if defined(_WIN32)
#define GFX_APIENTRY __stdcall
#else
#define GFX_APIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_ZERO 0
#define GL_ONE 1
#define GL_LINES 0x0001
#define GL_TRIANGLES 0x0004
#define GL_LEQUAL 0x0203
#define GL_ALWAYS 0x0207
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_DITHER 0x0BD0
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_RGBA8 0x8058
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE0 0x84C0
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_MAX_SAMPLES 0x8D57
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#define GL_NO_ERROR 0
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_NONE 0
#define GL_LESS 0x0201
#define GL_DEPTH_COMPONENT 0x1902
#define GL_MULTISAMPLE 0x809D

/* X(return type, name without the gl prefix, parameter list) */
#define GFX_GL_FUNCTIONS(X) \
    X(void, ActiveTexture, (GLenum texture)) \
    X(void, AttachShader, (GLuint program, GLuint shader)) \
    X(void, BindBuffer, (GLenum target, GLuint buffer)) \
    X(void, BindFramebuffer, (GLenum target, GLuint framebuffer)) \
    X(void, BindRenderbuffer, (GLenum target, GLuint renderbuffer)) \
    X(void, BindTexture, (GLenum target, GLuint texture)) \
    X(void, BindVertexArray, (GLuint array)) \
    X(void, BlendFunc, (GLenum sfactor, GLenum dfactor)) \
    X(void, BlendFuncSeparate, (GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)) \
    X(void, BlitFramebuffer, (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, \
                              GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)) \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
    X(GLenum, CheckFramebufferStatus, (GLenum target)) \
    X(void, Clear, (GLbitfield mask)) \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void, ClearDepth, (GLdouble depth)) \
    X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a)) \
    X(void, CompileShader, (GLuint shader)) \
    X(GLuint, CreateProgram, (void)) \
    X(GLuint, CreateShader, (GLenum type)) \
    X(void, DeleteBuffers, (GLsizei n, const GLuint *buffers)) \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint *framebuffers)) \
    X(void, DeleteProgram, (GLuint program)) \
    X(void, DeleteRenderbuffers, (GLsizei n, const GLuint *renderbuffers)) \
    X(void, DeleteShader, (GLuint shader)) \
    X(void, DeleteTextures, (GLsizei n, const GLuint *textures)) \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint *arrays)) \
    X(void, DepthFunc, (GLenum func)) \
    X(void, DepthMask, (GLboolean flag)) \
    X(void, DrawBuffer, (GLenum buf)) \
    X(void, Disable, (GLenum cap)) \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count)) \
    X(void, Enable, (GLenum cap)) \
    X(void, EnableVertexAttribArray, (GLuint index)) \
    X(void, FramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget, GLuint renderbuffer)) \
    X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(void, GenBuffers, (GLsizei n, GLuint *buffers)) \
    X(void, GenFramebuffers, (GLsizei n, GLuint *framebuffers)) \
    X(void, GenRenderbuffers, (GLsizei n, GLuint *renderbuffers)) \
    X(void, GenTextures, (GLsizei n, GLuint *textures)) \
    X(void, GenVertexArrays, (GLsizei n, GLuint *arrays)) \
    X(GLenum, GetError, (void)) \
    X(void, GetIntegerv, (GLenum pname, GLint *data)) \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei size, GLsizei *length, GLchar *log)) \
    X(void, GetProgramiv, (GLuint program, GLenum pname, GLint *param)) \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei *length, GLchar *log)) \
    X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint *param)) \
    X(const GLubyte *, GetString, (GLenum name)) \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar *name)) \
    X(void, LinkProgram, (GLuint program)) \
    X(void, PixelStorei, (GLenum pname, GLint param)) \
    X(void, PolygonOffset, (GLfloat factor, GLfloat units)) \
    X(void, ReadBuffer, (GLenum src)) \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels)) \
    X(void, RenderbufferStorage, (GLenum target, GLenum format, GLsizei w, GLsizei h)) \
    X(void, RenderbufferStorageMultisample, (GLenum target, GLsizei samples, GLenum format, GLsizei w, GLsizei h)) \
    X(void, Scissor, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)) \
    X(void, TexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border, \
                         GLenum format, GLenum type, const void *pixels)) \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param)) \
    X(void, TexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, \
                            GLenum type, const void *pixels)) \
    X(void, Uniform1f, (GLint location, GLfloat v0)) \
    X(void, Uniform1i, (GLint location, GLint v0)) \
    X(void, Uniform4fv, (GLint location, GLsizei count, const GLfloat *value)) \
    X(void, Uniform4iv, (GLint location, GLsizei count, const GLint *value)) \
    X(void, UseProgram, (GLuint program)) \
    X(void, VertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, \
                                  const void *pointer)) \
    X(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h))

#define GFX_GL_DECLARE(ret, name, args) \
    typedef ret(GFX_APIENTRY *gfx_PFN_##name) args; \
    extern gfx_PFN_##name gfx_gl##name;
GFX_GL_FUNCTIONS(GFX_GL_DECLARE)
#undef GFX_GL_DECLARE

/* Loads every function. Returns 0 when all were found, otherwise -1 (and logs the missing names). */
int gfx_gl_load(void);

#define glActiveTexture gfx_glActiveTexture
#define glAttachShader gfx_glAttachShader
#define glBindBuffer gfx_glBindBuffer
#define glBindFramebuffer gfx_glBindFramebuffer
#define glBindRenderbuffer gfx_glBindRenderbuffer
#define glBindTexture gfx_glBindTexture
#define glBindVertexArray gfx_glBindVertexArray
#define glBlendFunc gfx_glBlendFunc
#define glBlendFuncSeparate gfx_glBlendFuncSeparate
#define glBlitFramebuffer gfx_glBlitFramebuffer
#define glBufferData gfx_glBufferData
#define glCheckFramebufferStatus gfx_glCheckFramebufferStatus
#define glClear gfx_glClear
#define glClearColor gfx_glClearColor
#define glClearDepth gfx_glClearDepth
#define glColorMask gfx_glColorMask
#define glCompileShader gfx_glCompileShader
#define glCreateProgram gfx_glCreateProgram
#define glCreateShader gfx_glCreateShader
#define glDeleteBuffers gfx_glDeleteBuffers
#define glDeleteFramebuffers gfx_glDeleteFramebuffers
#define glDeleteProgram gfx_glDeleteProgram
#define glDeleteRenderbuffers gfx_glDeleteRenderbuffers
#define glDeleteShader gfx_glDeleteShader
#define glDeleteTextures gfx_glDeleteTextures
#define glDeleteVertexArrays gfx_glDeleteVertexArrays
#define glDepthFunc gfx_glDepthFunc
#define glDepthMask gfx_glDepthMask
#define glDrawBuffer gfx_glDrawBuffer
#define glDisable gfx_glDisable
#define glDrawArrays gfx_glDrawArrays
#define glEnable gfx_glEnable
#define glEnableVertexAttribArray gfx_glEnableVertexAttribArray
#define glFramebufferRenderbuffer gfx_glFramebufferRenderbuffer
#define glFramebufferTexture2D gfx_glFramebufferTexture2D
#define glGenBuffers gfx_glGenBuffers
#define glGenFramebuffers gfx_glGenFramebuffers
#define glGenRenderbuffers gfx_glGenRenderbuffers
#define glGenTextures gfx_glGenTextures
#define glGenVertexArrays gfx_glGenVertexArrays
#define glGetError gfx_glGetError
#define glGetIntegerv gfx_glGetIntegerv
#define glGetProgramInfoLog gfx_glGetProgramInfoLog
#define glGetProgramiv gfx_glGetProgramiv
#define glGetShaderInfoLog gfx_glGetShaderInfoLog
#define glGetShaderiv gfx_glGetShaderiv
#define glGetString gfx_glGetString
#define glGetUniformLocation gfx_glGetUniformLocation
#define glLinkProgram gfx_glLinkProgram
#define glPixelStorei gfx_glPixelStorei
#define glPolygonOffset gfx_glPolygonOffset
#define glReadBuffer gfx_glReadBuffer
#define glReadPixels gfx_glReadPixels
#define glRenderbufferStorage gfx_glRenderbufferStorage
#define glRenderbufferStorageMultisample gfx_glRenderbufferStorageMultisample
#define glScissor gfx_glScissor
#define glShaderSource gfx_glShaderSource
#define glTexImage2D gfx_glTexImage2D
#define glTexParameteri gfx_glTexParameteri
#define glTexSubImage2D gfx_glTexSubImage2D
#define glUniform1f gfx_glUniform1f
#define glUniform1i gfx_glUniform1i
#define glUniform4fv gfx_glUniform4fv
#define glUniform4iv gfx_glUniform4iv
#define glUseProgram gfx_glUseProgram
#define glVertexAttribPointer gfx_glVertexAttribPointer
#define glViewport gfx_glViewport

#endif
