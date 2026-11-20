/* Overlays for the Developer menu: the camera of the latest 3D scene and lines in world space drawn over it.

   The lines are drawn into the frame's own render target after the game's display lists, so they share its
   resolution, its multisampling and its depth buffer. With the depth test on, a line that lies behind a surface of
   the picture is hidden. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_host.h"

#include "gfx_internal.h"

/* A vertex of a line: clip-space position and colour. */
typedef struct LineVertex {
    float x, y, z, w;
    float r, g, b, a;
} LineVertex;

typedef struct ClipPoint {
    float x, y, z, w;
} ClipPoint;

static struct {
    GfxDebugLine *lines;
    int count, cap;
    int depth;
    LineVertex *verts;
    int vertCap;
    GLuint program, vao, vbo;
    int failed;
} D;

static const char LINE_VERTEX_SHADER[] =
    "#version 330 core\n"
    "layout(location = 0) in vec4 aPos;\n"
    "layout(location = 1) in vec4 aColor;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    gl_Position = aPos;\n"
    "    vColor = aColor;\n"
    "}\n";

static const char LINE_FRAGMENT_SHADER[] =
    "#version 330 core\n"
    "in vec4 vColor;\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = vColor;\n"
    "}\n";

int gfx_scene_view(GfxSceneView *v) {
    *v = gfxScene;
    return gfxScene.valid;
}

int gfx_scene_project(const GfxSceneView *v, float x, float y, float z, float *sx, float *sy, float *depth) {
    const float *m = v->vp;
    float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
    float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
    float cz = x * m[2] + y * m[6] + z * m[10] + m[14];
    float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
    float px;
    float py;

    if (!v->valid || cw <= 0.001f || cz < -cw) {
        return 0;
    }
    /* The N64 screen is 320x240; the picture fills frameW x frameH of the window. */
    px = (v->vpTrans[0] + cx / cw * v->vpScale[0]) / 320.0f;
    py = (v->vpTrans[1] - cy / cw * v->vpScale[1]) / 240.0f;
    *sx = v->frameX + px * v->frameW;
    *sy = v->frameY + py * v->frameH;
    *depth = cz / cw;
    return 1;
}

void gfx_debug_lines(const GfxDebugLine *lines, int count, int depth) {
    if (count > D.cap) {
        GfxDebugLine *n = realloc(D.lines, (size_t)count * sizeof(*n));

        if (n == NULL) {
            D.count = 0;
            return;
        }
        D.lines = n;
        D.cap = count;
    }
    if (count > 0) {
        memcpy(D.lines, lines, (size_t)count * sizeof(*lines));
    }
    D.count = count > 0 ? count : 0;
    D.depth = depth;
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int make_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, LINE_VERTEX_SHADER);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, LINE_FRAGMENT_SHADER);
    GLint ok = 0;

    if (vs == 0 || fs == 0) {
        return 0;
    }
    D.program = glCreateProgram();
    glAttachShader(D.program, vs);
    glAttachShader(D.program, fs);
    glLinkProgram(D.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(D.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(D.program);
        D.program = 0;
        return 0;
    }
    glGenVertexArrays(1, &D.vao);
    glGenBuffers(1, &D.vbo);
    glBindVertexArray(D.vao);
    glBindBuffer(GL_ARRAY_BUFFER, D.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex), (const void *)offsetof(LineVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex), (const void *)offsetof(LineVertex, r));
    glBindVertexArray(0);
    return 1;
}

/* The camera's place in the world: the point whose clip-space x, y and w are all 0. Returns 0 when there is none. */
static int camera_position(const float *m, float *e) {
    /* Solve [x y z 1] * column k = 0 for the columns 0, 1 and 3 of the matrix (Gauss-Jordan elimination). */
    static const int cols[3] = {0, 1, 3};
    float a[3][4];
    int i;
    int j;
    int k;

    for (i = 0; i < 3; i++) {
        a[i][0] = m[0 * 4 + cols[i]];
        a[i][1] = m[1 * 4 + cols[i]];
        a[i][2] = m[2 * 4 + cols[i]];
        a[i][3] = -m[3 * 4 + cols[i]];
    }
    for (i = 0; i < 3; i++) {
        int best = i;

        for (j = i + 1; j < 3; j++) {
            if (fabsf(a[j][i]) > fabsf(a[best][i])) {
                best = j;
            }
        }
        if (fabsf(a[best][i]) < 1e-9f) {
            return 0;
        }
        for (k = 0; k < 4; k++) {
            float t = a[i][k];

            a[i][k] = a[best][k];
            a[best][k] = t;
        }
        for (j = 0; j < 3; j++) {
            if (j != i) {
                float f = a[j][i] / a[i][i];

                for (k = i; k < 4; k++) {
                    a[j][k] -= f * a[i][k];
                }
            }
        }
    }
    for (i = 0; i < 3; i++) {
        e[i] = a[i][3] / a[i][i];
    }
    return 1;
}

static ClipPoint to_clip(const float *m, const float *p) {
    ClipPoint c;

    c.x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    c.y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    c.z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
    c.w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
    return c;
}

static void put_vertex(LineVertex *v, ClipPoint c, unsigned int color) {
    /* The same mapping as load_vertices in gfx_dl.c: the N64 viewport onto OpenGL's clip space. */
    v->x = c.x * (gfxScene.vpScale[0] / (SCREEN_W / 2)) + c.w * (gfxScene.vpTrans[0] / (SCREEN_W / 2) - 1.0f);
    v->y = c.y * (gfxScene.vpScale[1] / (SCREEN_H / 2)) + c.w * (1.0f - gfxScene.vpTrans[1] / (SCREEN_H / 2));
    v->z = c.z;
    v->w = c.w;
    v->r = (float)(color & 255) / 255.0f;
    v->g = (float)((color >> 8) & 255) / 255.0f;
    v->b = (float)((color >> 16) & 255) / 255.0f;
    v->a = (float)((color >> 24) & 255) / 255.0f;
}

void gfx_debug_draw(GLuint fbo, int width, int height) {
    const float *m = gfxScene.vp;
    float eye[3];
    int haveEye;
    int n = 0;
    int i;

    if (D.count <= 0 || !gfxScene.valid || D.failed) {
        return;
    }
    if (D.program == 0 && !make_program()) {
        D.failed = 1;
        gfx_log("gfx: the debug line shader did not build; no overlay lines will be drawn");
        return;
    }
    if (D.vertCap < D.count * 2) {
        LineVertex *nv = realloc(D.verts, (size_t)D.count * 2 * sizeof(*nv));

        if (nv == NULL) {
            return;
        }
        D.verts = nv;
        D.vertCap = D.count * 2;
    }
    haveEye = camera_position(m, eye);
    for (i = 0; i < D.count; i++) {
        ClipPoint c[2];
        float da;
        float db;
        int k;

        for (k = 0; k < 2; k++) {
            float p[3];

            memcpy(p, k ? D.lines[i].b : D.lines[i].a, sizeof(p));
            if (haveEye) {
                /* A point moved along its line of sight to the camera keeps its place in the picture. A small
                   pull keeps a line that lies in a surface from fighting with that surface in the depth test. */
                float dx = eye[0] - p[0];
                float dy = eye[1] - p[1];
                float dz = eye[2] - p[2];
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                float pull = dist > 1.0f ? 0.003f + 0.5f / dist : 0.0f;

                p[0] += dx * pull;
                p[1] += dy * pull;
                p[2] += dz * pull;
            }
            c[k] = to_clip(m, p);
        }
        /* Clip against the near plane (z = -w) as the renderer does. */
        da = c[0].z + c[0].w;
        db = c[1].z + c[1].w;
        if (da < 0.0f && db < 0.0f) {
            continue;
        }
        if (da < 0.0f || db < 0.0f) {
            float t = da / (da - db);
            ClipPoint *out = da < 0.0f ? &c[0] : &c[1];
            ClipPoint cut;

            cut.x = c[0].x + (c[1].x - c[0].x) * t;
            cut.y = c[0].y + (c[1].y - c[0].y) * t;
            cut.z = c[0].z + (c[1].z - c[0].z) * t;
            cut.w = c[0].w + (c[1].w - c[0].w) * t;
            *out = cut;
        }
        put_vertex(&D.verts[n++], c[0], D.lines[i].color);
        put_vertex(&D.verts[n++], c[1], D.lines[i].color);
    }
    if (n == 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (D.depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(D.program);
    glBindVertexArray(D.vao);
    glBindBuffer(GL_ARRAY_BUFFER, D.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * sizeof(LineVertex), D.verts, GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, n);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
}

void gfx_debug_shutdown(void) {
    if (D.program) {
        glDeleteProgram(D.program);
    }
    if (D.vao) {
        glDeleteVertexArrays(1, &D.vao);
    }
    if (D.vbo) {
        glDeleteBuffers(1, &D.vbo);
    }
    free(D.lines);
    free(D.verts);
    memset(&D, 0, sizeof(D));
}
