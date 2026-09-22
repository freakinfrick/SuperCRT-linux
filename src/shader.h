// GLSL program compilation helpers.
#pragma once

#include <GL/gl.h>

// Compiles and links vs + fs.  Returns 0 on failure (with the compiler log on stderr).
GLuint Shader_Build(const char *vs_source, const char *fs_source, const char *name);
