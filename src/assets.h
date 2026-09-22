// Asset loaders for the CC0 CRTSim data files: uncompressed BMP textures and .m3d meshes.
#pragma once

#include <stdint.h>

typedef struct {
    int width, height;
    unsigned char *rgba; // top-down RGBA8, row 0 = top of the image
} Bitmap;

// Loads an uncompressed 24/32-bit BMP.  Returns 1 on success.
int Bitmap_LoadBMP(const char *path, Bitmap *out);
void Bitmap_Free(Bitmap *b);

// .m3d stream usages, from the reference implementation's M3D.h.
enum {
    M3D_POSITION = 0,
    M3D_NORMAL = 1,
    M3D_TANGENT = 2,
    M3D_BINORMAL = 3,
    M3D_COLOR = 4,
    M3D_TEXCOORD0 = 5,
    M3D_TEXCOORD1 = 6,
};

typedef struct {
    int num_vertices, num_indices;
    float *positions;      // 3 per vertex
    float *normals;        // 3 per vertex
    unsigned char *colors; // 4 per vertex, RGBA (reordered from D3DCOLOR's BGRA bytes)
    float *uv0;            // 2 per vertex
    float *uv1;            // 1 per vertex, or NULL when absent
    uint16_t *indices;
} Mesh;

// Loads the mesh streams this port needs, de-interleaving them into GL attribute
// arrays.  Returns 1 on success.
int Mesh_LoadM3D(const char *path, Mesh *out);
void Mesh_Free(Mesh *m);
