/* Host test for the real (non-simulator) Wi-Fi command formatting. */
#define _GNU_SOURCE 1
#include <assert.h>
#include <string.h>
#include "../../apps/sloppy/sloppy_net.c"

int main(void)
{
    char out[140];
    assert(wpa_quote(out, sizeof out, "Cafe \"Guest\"\\5G"));
    assert(!strcmp(out, "\"Cafe \\\"Guest\\\"\\\\5G\""));
    assert(!wpa_quote(out, 5, "long network"));

    wpa_unescape(out, sizeof out, "Cafe\\x20Guest", strlen("Cafe\\x20Guest"));
    assert(!strcmp(out, "Cafe Guest"));
    return 0;
}
