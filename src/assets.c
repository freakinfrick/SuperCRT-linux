// See assets.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"

static uint32_t RdU32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t RdU16(const unsigned char *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

static unsigned char *ReadWholeFile(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "supercrt: cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        fprintf(stderr, "supercrt: %s is empty\n", path);
        return NULL;
    }
    unsigned char *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        fprintf(stderr, "supercrt: cannot read %s\n", path);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return data;
}

void Bitmap_Free(Bitmap *b)
{
    free(b->rgba);
    b->rgba = NULL;
    b->width = b->height = 0;
}

int Bitmap_LoadBMP(const char *path, Bitmap *out)
{
    memset(out, 0, sizeof(*out));

    size_t size = 0;
    unsigned char *file = ReadWholeFile(path, &size);
    if (!file) {
        return 0;
    }
    if (size < 54 || file[0] != 'B' || file[1] != 'M') {
        fprintf(stderr, "supercrt: %s is not a BMP\n", path);
        free(file);
        return 0;
    }

    uint32_t data_offset = RdU32(file + 10);
    uint32_t dib_size = RdU32(file + 14);
    int32_t width, height;
    uint16_t bpp;
    uint32_t compression;
    if (dib_size == 40) {
        width = (int32_t)RdU32(file + 18);
        height = (int32_t)RdU32(file + 22);
        bpp = RdU16(file + 28);
        compression = RdU32(file + 30);
    } else if (dib_size == 108 || dib_size == 124) {
        width = (int32_t)RdU32(file + 18);
        height = (int32_t)RdU32(file + 22);
        bpp = RdU16(file + 28);
        compression = RdU32(file + 30);
    } else {
        fprintf(stderr, "supercrt: %s: unsupported DIB header size %u\n", path, dib_size);
        free(file);
        return 0;
    }

    if (compression != 0 || (bpp != 24 && bpp != 32) || width <= 0 || height == 0) {
        fprintf(stderr, "supercrt: %s: unsupported BMP (%d bpp, compression %u)\n", path, bpp, compression);
        free(file);
        return 0;
    }
    int top_down = height < 0;
    if (top_down) {
        height = -height;
    }

    size_t src_stride = ((size_t)width * (bpp / 8) + 3) & ~(size_t)3;
    if (data_offset + src_stride * (size_t)height > size) {
        fprintf(stderr, "supercrt: %s: truncated BMP data\n", path);
        free(file);
        return 0;
    }

    unsigned char *rgba = malloc((size_t)width * (size_t)height * 4);
    if (!rgba) {
        free(file);
        return 0;
    }

    // BMP rows run bottom-up (unless the height was negative) and store BGR(A).  The GL
    // port uploads row 0 = top of the image, so flip while converting.
    for (int y = 0; y < height; ++y) {
        int src_row = top_down ? y : height - 1 - y;
        const unsigned char *src = file + data_offset + src_stride * (size_t)src_row;
        unsigned char *dst = rgba + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * (bpp / 8) + 2];
            dst[x * 4 + 1] = src[x * (bpp / 8) + 1];
            dst[x * 4 + 2] = src[x * (bpp / 8) + 0];
            dst[x * 4 + 3] = (bpp == 32) ? src[x * 4 + 3] : 0xff;
        }
    }

    free(file);
    out->width = width;
    out->height = height;
    out->rgba = rgba;
    return 1;
}

void Mesh_Free(Mesh *m)
{
    free(m->positions);
    free(m->normals);
    free(m->colors);
    free(m->uv0);
    free(m->uv1);
    free(m->indices);
    memset(m, 0, sizeof(*m));
}

int Mesh_LoadM3D(const char *path, Mesh *out)
{
    memset(out, 0, sizeof(*out));

    size_t size = 0;
    unsigned char *file = ReadWholeFile(path, &size);
    if (!file) {
        return 0;
    }

    if (size < 25 || memcmp(file, ".m3d", 4) != 0) {
        fprintf(stderr, "supercrt: %s is not an .m3d file\n", path);
        free(file);
        return 0;
    }
    const unsigned char major = file[4];
    const uint32_t num_streams = RdU32(file + 8);
    const uint32_t num_vertices = RdU32(file + 12);
    const uint32_t num_indices = RdU32(file + 16);
    const uint32_t index_size = RdU32(file + 21);

    if (major != 2 || index_size != 2 || num_streams == 0 || num_vertices == 0) {
        fprintf(stderr, "supercrt: %s: unsupported .m3d version %u (index size %u)\n", path, major, index_size);
        free(file);
        return 0;
    }

    size_t cursor = 25;
    if (cursor + (size_t)num_indices * 2 > size) {
        fprintf(stderr, "supercrt: %s: truncated index data\n", path);
        free(file);
        return 0;
    }

    uint16_t *indices = malloc((size_t)num_indices * sizeof(uint16_t));
    float *positions = calloc((size_t)num_vertices * 3, sizeof(float));
    float *normals = calloc((size_t)num_vertices * 3, sizeof(float));
    unsigned char *colors = calloc((size_t)num_vertices * 4, sizeof(unsigned char));
    float *uv0 = calloc((size_t)num_vertices * 2, sizeof(float));
    float *uv1 = NULL;
    if (!indices || !positions || !normals || !colors || !uv0) {
        goto fail;
    }

    for (uint32_t i = 0; i < num_indices; ++i) {
        indices[i] = RdU16(file + cursor + (size_t)i * 2);
    }
    cursor += (size_t)num_indices * 2;

    for (uint32_t stream = 0; stream < num_streams; ++stream) {
        if (cursor + 8 > size) {
            fprintf(stderr, "supercrt: %s: truncated stream header\n", path);
            goto fail;
        }
        uint32_t usage = RdU32(file + cursor);
        uint32_t stride = RdU32(file + cursor + 4);
        cursor += 8;
        if (cursor + (size_t)stride * num_vertices > size) {
            fprintf(stderr, "supercrt: %s: truncated stream %u data\n", path, stream);
            goto fail;
        }

        const unsigned char *src = file + cursor;
        for (uint32_t v = 0; v < num_vertices; ++v) {
            const unsigned char *elem = src + (size_t)v * stride;
            switch (usage) {
            case M3D_POSITION:
                if (stride < 12) goto fail;
                memcpy(&positions[(size_t)v * 3], elem, 12);
                break;
            case M3D_NORMAL:
                if (stride < 12) goto fail;
                memcpy(&normals[(size_t)v * 3], elem, 12);
                break;
            case M3D_COLOR:
                if (stride < 4) goto fail;
                // D3DCOLOR is 0xAARRGGBB, so the bytes on disk are B,G,R,A; GL reads
                // attribute bytes as R,G,B,A, hence the swap.
                colors[(size_t)v * 4 + 0] = elem[2];
                colors[(size_t)v * 4 + 1] = elem[1];
                colors[(size_t)v * 4 + 2] = elem[0];
                colors[(size_t)v * 4 + 3] = elem[3];
                break;
            case M3D_TEXCOORD0:
                if (stride < 8) goto fail;
                memcpy(&uv0[(size_t)v * 2], elem, 8);
                break;
            case M3D_TEXCOORD1:
                if (!uv1) {
                    uv1 = calloc(num_vertices, sizeof(float));
                    if (!uv1) goto fail;
                }
                if (stride < 4) goto fail;
                memcpy(&uv1[v], elem, 4);
                break;
            default:
                break; // tangents/binormals are unused by the CRT meshes
            }
        }
        cursor += (size_t)stride * num_vertices;
    }

    free(file);
    out->num_vertices = (int)num_vertices;
    out->num_indices = (int)num_indices;
    out->positions = positions;
    out->normals = normals;
    out->colors = colors;
    out->uv0 = uv0;
    out->uv1 = uv1;
    out->indices = indices;
    return 1;

fail:
    free(file);
    free(indices);
    free(positions);
    free(normals);
    free(colors);
    free(uv0);
    free(uv1);
    return 0;
}
