/* test_des.c - vncdes の既知の値による検証 */
#include "../src/vncdes.h"
#include <stdio.h>
#include <string.h>

static int check(const char *name, const unsigned char *got, const char *hex, int n)
{
    char buf[64];
    int i;
    for (i = 0; i < n; i++) sprintf(buf + i * 2, "%02x", got[i]);
    printf("%-34s %s %s\n", name, buf, strcmp(buf, hex) ? "NG" : "OK");
    return strcmp(buf, hex) != 0;
}

int main(void)
{
    /* FIPS の例: 鍵 133457799BBCDFF1, 平文 0123456789ABCDEF -> 85E813540F0AB405 */
    static const unsigned char key[8] = { 0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1 };
    static const unsigned char pt[8]  = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
    unsigned char out[16];
    char back[9];
    int bad = 0;

    vncdes_test_block(key, pt, out);
    bad += check("標準 DES", out, "85e813540f0ab405", 8);
    /* vncpasswd で "password" を隠した値はよく知られた dbd83cfd727a1458 */
    vncdes_obfuscate("password", out);
    bad += check("隠したパスワード(password)", out, "dbd83cfd727a1458", 8);
    vncdes_reveal(out, back);
    printf("%-34s %s %s\n", "戻したパスワード", back, strcmp(back, "password") ? "NG" : "OK");
    bad += strcmp(back, "password") != 0;
    return bad;
}
