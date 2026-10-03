/* jpegenc.h - Tight の JPEG 用のベースライン JPEG 符号化 */
#ifndef JPEGENC_H
#define JPEGENC_H

#include <stddef.h>

enum { JPE_444 = 0, JPE_422 = 1, JPE_420 = 2 };

typedef struct JpegEnc JpegEnc;         /* スレッドごとに 1 つ */

JpegEnc *jpe_new(void);
void     jpe_free(JpegEnc *e);

/* 出力に必要な大きさの上限 */
size_t jpe_bound(int w, int h);

/* BGRX(1 画素 4 バイト)の画素を JPEG(JFIF)にする。
 * quality は 1..100、subsamp は JPE_444 / JPE_422 / JPE_420。
 * 戻り値は大きさ。cap に収まらなければ 0。 */
size_t jpe_encode(JpegEnc *e, const unsigned char *px, int stride, int w, int h,
                  int quality, int subsamp, unsigned char *out, size_t cap);

#endif
