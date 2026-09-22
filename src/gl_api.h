// Minimal OpenGL 2.0+/FBO entry point loader, resolved through glXGetProcAddress.
//
// GL 1.1 functions come straight from libGL (linked).  Everything newer is fetched at
// runtime into glptr_* variables, and short aliases below keep call sites reading like
// ordinary GL code.
#pragma once

#include <GL/gl.h>
#include <GL/glext.h>

#define SUPERCRT_GL_FUNCS(X)                                        \
    X(PFNGLACTIVETEXTUREPROC,             glActiveTexture)          \
    X(PFNGLGENBUFFERSPROC,                glGenBuffers)             \
    X(PFNGLBINDBUFFERPROC,                glBindBuffer)             \
    X(PFNGLBUFFERDATAPROC,                glBufferData)             \
    X(PFNGLBUFFERSUBDATAPROC,             glBufferSubData)          \
    X(PFNGLDELETEBUFFERSPROC,             glDeleteBuffers)          \
    X(PFNGLGENVERTEXARRAYSPROC,           glGenVertexArrays)        \
    X(PFNGLBINDVERTEXARRAYPROC,           glBindVertexArray)        \
    X(PFNGLDELETEVERTEXARRAYSPROC,        glDeleteVertexArrays)     \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC,   glEnableVertexAttribArray)\
    X(PFNGLVERTEXATTRIBPOINTERPROC,       glVertexAttribPointer)    \
    X(PFNGLCREATESHADERPROC,              glCreateShader)           \
    X(PFNGLSHADERSOURCEPROC,              glShaderSource)           \
    X(PFNGLCOMPILESHADERPROC,             glCompileShader)          \
    X(PFNGLGETSHADERIVPROC,               glGetShaderiv)            \
    X(PFNGLGETSHADERINFOLOGPROC,          glGetShaderInfoLog)       \
    X(PFNGLCREATEPROGRAMPROC,             glCreateProgram)          \
    X(PFNGLATTACHSHADERPROC,              glAttachShader)           \
    X(PFNGLLINKPROGRAMPROC,               glLinkProgram)            \
    X(PFNGLGETPROGRAMIVPROC,              glGetProgramiv)           \
    X(PFNGLGETPROGRAMINFOLOGPROC,         glGetProgramInfoLog)      \
    X(PFNGLUSEPROGRAMPROC,                glUseProgram)             \
    X(PFNGLDELETESHADERPROC,              glDeleteShader)           \
    X(PFNGLDELETEPROGRAMPROC,             glDeleteProgram)          \
    X(PFNGLGETUNIFORMLOCATIONPROC,        glGetUniformLocation)     \
    X(PFNGLUNIFORM1FPROC,                 glUniform1f)              \
    X(PFNGLUNIFORM1IPROC,                 glUniform1i)              \
    X(PFNGLUNIFORM2FPROC,                 glUniform2f)              \
    X(PFNGLUNIFORM3FPROC,                 glUniform3f)              \
    X(PFNGLUNIFORM4FPROC,                 glUniform4f)              \
    X(PFNGLUNIFORM2FVPROC,                glUniform2fv)             \
    X(PFNGLUNIFORM3FVPROC,                glUniform3fv)             \
    X(PFNGLUNIFORM4FVPROC,                glUniform4fv)             \
    X(PFNGLUNIFORMMATRIX4FVPROC,          glUniformMatrix4fv)       \
    X(PFNGLGENFRAMEBUFFERSPROC,           glGenFramebuffers)        \
    X(PFNGLDELETEFRAMEBUFFERSPROC,        glDeleteFramebuffers)     \
    X(PFNGLBINDFRAMEBUFFERPROC,           glBindFramebuffer)        \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC,      glFramebufferTexture2D)   \
    X(PFNGLGENRENDERBUFFERSPROC,          glGenRenderbuffers)       \
    X(PFNGLDELETERENDERBUFFERSPROC,       glDeleteRenderbuffers)    \
    X(PFNGLBINDRENDERBUFFERPROC,          glBindRenderbuffer)       \
    X(PFNGLRENDERBUFFERSTORAGEPROC,       glRenderbufferStorage)    \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC,   glFramebufferRenderbuffer)\
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,    glCheckFramebufferStatus) \
    X(PFNGLDRAWBUFFERSPROC,               glDrawBuffers)          \
    X(PFNGLGENSAMPLERSPROC,               glGenSamplers)          \
    X(PFNGLDELETESAMPLERSPROC,            glDeleteSamplers)       \
    X(PFNGLBINDSAMPLERPROC,               glBindSampler)          \
    X(PFNGLSAMPLERPARAMETERIPROC,         glSamplerParameteri)    \
    X(PFNGLSAMPLERPARAMETERFVPROC,        glSamplerParameterfv)

#define X(type, name) extern type glptr_##name;
SUPERCRT_GL_FUNCS(X)
#undef X

// Aliases.  Kept in sync with SUPERCRT_GL_FUNCS by hand; a missing alias is a compile
// error at the call site (gl.h does not declare these 2.0+ entry points), so drift is
// caught by the compiler rather than at runtime.
#define glActiveTexture             glptr_glActiveTexture
#define glGenBuffers                glptr_glGenBuffers
#define glBindBuffer                glptr_glBindBuffer
#define glBufferData                glptr_glBufferData
#define glBufferSubData             glptr_glBufferSubData
#define glDeleteBuffers             glptr_glDeleteBuffers
#define glGenVertexArrays           glptr_glGenVertexArrays
#define glBindVertexArray           glptr_glBindVertexArray
#define glDeleteVertexArrays        glptr_glDeleteVertexArrays
#define glEnableVertexAttribArray   glptr_glEnableVertexAttribArray
#define glVertexAttribPointer       glptr_glVertexAttribPointer
#define glCreateShader              glptr_glCreateShader
#define glShaderSource              glptr_glShaderSource
#define glCompileShader             glptr_glCompileShader
#define glGetShaderiv               glptr_glGetShaderiv
#define glGetShaderInfoLog          glptr_glGetShaderInfoLog
#define glCreateProgram             glptr_glCreateProgram
#define glAttachShader              glptr_glAttachShader
#define glLinkProgram               glptr_glLinkProgram
#define glGetProgramiv              glptr_glGetProgramiv
#define glGetProgramInfoLog         glptr_glGetProgramInfoLog
#define glUseProgram                glptr_glUseProgram
#define glDeleteShader              glptr_glDeleteShader
#define glDeleteProgram             glptr_glDeleteProgram
#define glGetUniformLocation        glptr_glGetUniformLocation
#define glUniform1f                 glptr_glUniform1f
#define glUniform1i                 glptr_glUniform1i
#define glUniform2f                 glptr_glUniform2f
#define glUniform3f                 glptr_glUniform3f
#define glUniform4f                 glptr_glUniform4f
#define glUniform2fv                glptr_glUniform2fv
#define glUniform3fv                glptr_glUniform3fv
#define glUniform4fv                glptr_glUniform4fv
#define glUniformMatrix4fv          glptr_glUniformMatrix4fv
#define glGenFramebuffers           glptr_glGenFramebuffers
#define glDeleteFramebuffers        glptr_glDeleteFramebuffers
#define glBindFramebuffer           glptr_glBindFramebuffer
#define glFramebufferTexture2D      glptr_glFramebufferTexture2D
#define glGenRenderbuffers          glptr_glGenRenderbuffers
#define glDeleteRenderbuffers       glptr_glDeleteRenderbuffers
#define glBindRenderbuffer          glptr_glBindRenderbuffer
#define glRenderbufferStorage       glptr_glRenderbufferStorage
#define glFramebufferRenderbuffer   glptr_glFramebufferRenderbuffer
#define glCheckFramebufferStatus    glptr_glCheckFramebufferStatus
#define glDrawBuffers               glptr_glDrawBuffers
#define glGenSamplers               glptr_glGenSamplers
#define glDeleteSamplers            glptr_glDeleteSamplers
#define glBindSampler               glptr_glBindSampler
#define glSamplerParameteri         glptr_glSamplerParameteri
#define glSamplerParameterfv        glptr_glSamplerParameterfv

// Resolves every entry point above.  Returns 0 and reports the first missing name on
// failure.
int Gl_Load(void *(*get_proc)(const char *name));

// Logs (and returns) the current GL error, tagged with a call-site description.
unsigned Gl_Check(const char *where);

const char *Gl_ErrorString(unsigned err);
