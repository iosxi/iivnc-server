/* vncdes.h - VNC 認証の DES(iivnc-server と iivnc-client で同じもの) */
#ifndef VNCDES_H
#define VNCDES_H

/* VNC 認証の応答。パスワードは 8 バイトまで使う */
void vncdes_response(const char *password, const unsigned char challenge[16], unsigned char response[16]);

/* ini に置くためにパスワードを隠す / 戻す(ほかの VNC と同じ固定の鍵) */
void vncdes_obfuscate(const char *password, unsigned char out[8]);
void vncdes_reveal(const unsigned char in[8], char password[9]);

/* 検証用: 標準の DES で 1 ブロック暗号化する */
void vncdes_test_block(const unsigned char key[8], const unsigned char in[8], unsigned char out[8]);

#endif
