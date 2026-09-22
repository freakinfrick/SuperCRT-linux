// See shader.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_api.h"
#include "shader.h"

static GLuint CompileStage(GLenum stage, const char *source, const char *name)
{
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        char *log = malloc((size_t)(len > 1 ? len : 1));
        glGetShaderInfoLog(shader, len, NULL, log);
        fprintf(stderr, "supercrt: %s %s shader failed to compile:\n%s\n", name,
                stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        free(log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint Shader_Build(const char *vs_source, const char *fs_source, const char *name)
{
    GLuint vs = CompileStage(GL_VERTEX_SHADER, vs_source, name);
    if (!vs) {
        return 0;
    }
    GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fs_source, name);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
        char *log = malloc((size_t)(len > 1 ? len : 1));
        glGetProgramInfoLog(program, len, NULL, log);
        fprintf(stderr, "supercrt: %s program failed to link:\n%s\n", name, log);
        free(log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}
