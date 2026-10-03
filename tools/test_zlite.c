/* test_zlite.c - zdeflate / zinflate の検証(tools/test_zlite.py から呼ぶ)
 *
 *   test_zlite comp <in> <out> <level> <chunk>
 *       in を chunk バイトずつ、直前 32KB を辞書にして独立に圧縮し、
 *       つないで 1 本の zlib ストリームとして out へ書く。速度を表示する。
 *   test_zlite infl <in.z> <lens> <out>
 *       lens(各区切りの「圧縮後の長さ 展開後の長さ」の行)に従って
 *       区切りごとに zi_inflate し、out へ書く。区切りの入力は半分ずつ
 *       与え、出力も 2 回に分けて取り出す(再開の確認)。
 *   test_zlite round <in> <level> <chunk>
 *       圧縮して自分で展開し、元と一致するか確かめる。
 */
#include "../src/zlite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;
    long n;
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    p = (unsigned char *)malloc((size_t)n + 1);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(1); }
    fclose(f);
    *len = (size_t)n;
    return p;
}

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static size_t out_cap(size_t len, size_t chunk) { return (len / chunk + 1) * zd_bound(chunk) + 4096; }

static size_t compress_all(const unsigned char *src, size_t len, unsigned char *out, int level, size_t chunk, FILE *lens)
{
    ZDWork *w = zd_work_new();
    size_t pos = 0, o = 0;
    while (pos < len) {
        size_t n = len - pos < chunk ? len - pos : chunk;
        size_t dict = pos < ZD_WINDOW ? pos : ZD_WINDOW;
        size_t c = zd_compress(w, src + pos - dict, dict, n, out + o, level, pos == 0);
        if (c > zd_bound(n)) { fprintf(stderr, "bound exceeded %zu > %zu\n", c, zd_bound(n)); exit(1); }
        if (lens) fprintf(lens, "%zu %zu\n", c, n);
        o += c;
        pos += n;
    }
    zd_work_free(w);
    return o;
}

int main(int argc, char **argv)
{
    if (argc >= 6 && !strcmp(argv[1], "comp")) {
        size_t len, clen, i;
        unsigned char *src = read_file(argv[2], &len), *out;
        int level = atoi(argv[4]);
        size_t chunk = (size_t)atoll(argv[5]);
        double t0, best = 1e9;
        char lp[1024];
        FILE *lens, *f;
        out = (unsigned char *)malloc(out_cap(len, chunk));
        snprintf(lp, sizeof(lp), "%s.lens", argv[3]);
        lens = fopen(lp, "w");
        clen = compress_all(src, len, out, level, chunk, lens);
        fclose(lens);
        for (i = 0; i < 3; i++) {
            t0 = now_ms();
            compress_all(src, len, out, level, chunk, NULL);
            t0 = now_ms() - t0;
            if (t0 < best) best = t0;
        }
        f = fopen(argv[3], "wb");
        fwrite(out, 1, clen, f);
        fclose(f);
        printf("level %d chunk %zu: %zu -> %zu (%.1f%%) %.1f MB/s\n", level, chunk, len, clen,
               100.0 * (double)clen / (double)len, (double)len / 1048576.0 / (best / 1000.0));
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "infl")) {
        size_t len, pos = 0, total = 0;
        unsigned char *src = read_file(argv[2], &len), *out;
        FILE *lens = fopen(argv[3], "r"), *f;
        ZInflate *z = zi_new();
        size_t c, n;
        double t0 = now_ms();
        out = (unsigned char *)malloc(1 << 28);
        while (fscanf(lens, "%zu %zu", &c, &n) == 2) {
            size_t h = c / 2, k = n / 3;
            int r;
            r = zi_inflate(z, src + pos, h, out + total, 0);
            if (r) { printf("FAIL first half r=%d at %zu\n", r, pos); return 1; }
            r = zi_inflate(z, src + pos + h, c - h, out + total, k);
            if (r) { printf("FAIL part1 r=%d at %zu\n", r, pos); return 1; }
            r = zi_inflate(z, NULL, 0, out + total + k, n - k);
            if (r) { printf("FAIL part2 r=%d at %zu\n", r, pos); return 1; }
            pos += c;
            total += n;
        }
        t0 = now_ms() - t0;
        f = fopen(argv[4], "wb");
        fwrite(out, 1, total, f);
        fclose(f);
        printf("inflated %zu -> %zu  %.1f MB/s\n", pos, total, (double)total / 1048576.0 / (t0 / 1000.0));
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "round")) {
        size_t len, clen;
        unsigned char *src = read_file(argv[2], &len), *out = (unsigned char *)malloc(out_cap(len, (size_t)atoll(argv[4])));
        unsigned char *back = (unsigned char *)malloc(len + 1);
        ZInflate *z = zi_new();
        double t0;
        int r;
        clen = compress_all(src, len, out, atoi(argv[3]), (size_t)atoll(argv[4]), NULL);
        t0 = now_ms();
        r = zi_inflate(z, out, clen, back, len);
        t0 = now_ms() - t0;
        if (r || memcmp(src, back, len)) { printf("FAIL round r=%d\n", r); return 1; }
        printf("round ok %zu -> %zu  inflate %.1f MB/s\n", len, clen, (double)len / 1048576.0 / (t0 / 1000.0));
        return 0;
    }
    fprintf(stderr, "usage: see source\n");
    return 2;
}
