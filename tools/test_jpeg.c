/* test_jpeg.c - jpegenc の検証と、WIC の JPEG 展開の速さ(tools/test_jpeg.py から呼ぶ)
 *
 *   test_jpeg enc <in.bgrx> <w> <h> <quality> <subsamp> <out.jpg> [tile]
 *       tile を指定すると tile×tile ずつ別々の JPEG にして時間と合計の大きさを測る
 *       (out.jpg には先頭のタイルだけ書く)。
 *   test_jpeg wicdec <in.jpg> <回数>
 */
#define COBJMACROS
#include "../src/jpegenc.h"
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;
    long n;
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    p = (unsigned char *)malloc((size_t)n + 1);
    fread(p, 1, (size_t)n, f);
    fclose(f);
    *len = (size_t)n;
    return p;
}

int main(int argc, char **argv)
{
    if (argc >= 8 && !strcmp(argv[1], "enc")) {
        size_t len, cap, total = 0, first = 0;
        unsigned char *px = read_file(argv[2], &len), *out;
        int w = atoi(argv[3]), h = atoi(argv[4]), q = atoi(argv[5]), ss = atoi(argv[6]);
        int tile = argc >= 9 ? atoi(argv[8]) : 0, rep, x, y;
        JpegEnc *e = jpe_new();
        double best = 1e9;
        FILE *f;
        if (!tile) tile = w > h ? w : h;
        cap = jpe_bound(tile, tile);
        out = (unsigned char *)malloc(cap);
        for (rep = 0; rep < 5; rep++) {
            double t0 = now_ms();
            total = 0;
            for (y = 0; y < h; y += tile)
                for (x = 0; x < w; x += tile) {
                    int tw = w - x < tile ? w - x : tile, th = h - y < tile ? h - y : tile;
                    size_t n = jpe_encode(e, px + ((size_t)y * w + x) * 4, w * 4, tw, th, q, ss, out, cap);
                    if (!n) { printf("FAIL encode\n"); return 1; }
                    if (x == 0 && y == 0) {
                        first = n;
                        if (rep == 0) { f = fopen(argv[7], "wb"); fwrite(out, 1, n, f); fclose(f); }
                    }
                    total += n;
                }
            t0 = now_ms() - t0;
            if (t0 < best) best = t0;
        }
        printf("%zu %zu %.3f\n", total, first, best);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "wicdec")) {
        size_t len;
        unsigned char *jpg = read_file(argv[2], &len), *buf = NULL;
        int n = atoi(argv[3]), i;
        IWICImagingFactory *fac;
        UINT w = 0, h = 0;
        double t0;
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac))) return 1;
        t0 = now_ms();
        for (i = 0; i < n; i++) {
            IWICStream *st;
            IWICBitmapDecoder *dec;
            IWICBitmapFrameDecode *fr;
            IWICFormatConverter *cv;
            WICRect rc;
            IWICImagingFactory_CreateStream(fac, &st);
            IWICStream_InitializeFromMemory(st, jpg, (DWORD)len);
            if (FAILED(IWICImagingFactory_CreateDecoderFromStream(fac, (IStream *)st, NULL, WICDecodeMetadataCacheOnDemand, &dec))) { printf("FAIL dec\n"); return 1; }
            IWICBitmapDecoder_GetFrame(dec, 0, &fr);
            IWICBitmapFrameDecode_GetSize(fr, &w, &h);
            if (!buf) buf = (unsigned char *)malloc((size_t)w * h * 4);
            IWICImagingFactory_CreateFormatConverter(fac, &cv);
            IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom);
            rc.X = 0; rc.Y = 0; rc.Width = (INT)w; rc.Height = (INT)h;
            if (FAILED(IWICFormatConverter_CopyPixels(cv, &rc, w * 4, w * h * 4, buf))) { printf("FAIL copy\n"); return 1; }
            IWICFormatConverter_Release(cv);
            IWICBitmapFrameDecode_Release(fr);
            IWICBitmapDecoder_Release(dec);
            IWICStream_Release(st);
        }
        t0 = now_ms() - t0;
        printf("%ux%u %.3f ms/回  %.1f Mpix/s\n", w, h, t0 / n, (double)w * h * n / (t0 / 1000.0) / 1e6);
        return 0;
    }
    return 2;
}
