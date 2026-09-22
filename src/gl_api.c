// See gl_api.h.
#include <stdio.h>

#include "gl_api.h"

#define X(type, name) type glptr_##name;
SUPERCRT_GL_FUNCS(X)
#undef X

int Gl_Load(void *(*get_proc)(const char *name))
{
    static const struct { void **slot; const char *name; } table[] = {
#define X(type, name) { (void **)&glptr_##name, #name },
        SUPERCRT_GL_FUNCS(X)
#undef X
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        *table[i].slot = get_proc(table[i].name);
        if (!*table[i].slot) {
            fprintf(stderr, "supercrt: missing OpenGL entry point %s\n", table[i].name);
            return 0;
        }
    }
    return 1;
}

const char *Gl_ErrorString(unsigned err)
{
    switch (err) {
    case GL_NO_ERROR:          return "GL_NO_ERROR";
    case GL_INVALID_ENUM:      return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:     return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
    case GL_OUT_OF_MEMORY:     return "GL_OUT_OF_MEMORY";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
    default:                   return "GL_UNKNOWN_ERROR";
    }
}

unsigned Gl_Check(const char *where)
{
    unsigned err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "supercrt: GL error %s (%#x) at %s\n", Gl_ErrorString(err), err, where);
    }
    return err;
}
