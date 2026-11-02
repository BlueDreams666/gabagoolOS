#!/usr/bin/env python3
"""Compile production Bluetooth helpers with fake command/PCM services."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'apps/hiby_bluetooth.c').read_text()
def function(name):
    import re
    match = re.search(r'^(?:static )?[^;\n]*\b'+name+r'\([^;]*?\)\n\{', source, re.M)
    if not match: raise AssertionError(name)
    start=match.start(); pos=match.end(); depth=1
    while depth:
        depth += (source[pos]=='{') - (source[pos]=='}');pos+=1
    return source[start:pos]
head=r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define BT_NAME_LEN 80
#define HZ 100
struct bt_device { char mac[18], name[BT_NAME_LEN]; bool paired; };
static const char *output;
static int exit_status;
static char command[256];
static int logs;
static void hiby_debug_log(const char *format, ...) {(void)format;logs++;}
static FILE *test_popen(const char *cmd,const char *mode) {
    (void)mode;snprintf(command,sizeof command,"%s",cmd);
    FILE *f=tmpfile();assert(f);fputs(output,f);rewind(f);return f;
}
static int test_pclose(FILE *f) {fclose(f);return exit_status;}
#define popen test_popen
#define pclose test_pclose
static const char *bt_playback_dev;
static char bt_active_codec[16];
static bool ready=true, routed, local;
static const int BT_REMOTE_INPUT_IDX=4;
static const char *bt_make_bt_playback_dev(const char *m,const char *c){(void)m;(void)c;return "bluealsa:test";}
static bool bt_wait_for_bluealsa_pcm(const char *m,int t){(void)m;(void)t;return ready;}
static void bt_route_to_local(void){local=true;}
static void bt_set_active_codec(const char *m){(void)m;bt_active_codec[0]=0;}
static int pcm_alsa_switch_playback_device(const char *d){(void)d;routed=true;return 0;}
static void hiby_pcm_set_bt_mac(const char *m){(void)m;}
static void bt_kick_audio_if_playing(void){}
static void button_add_input_device(int n){(void)n;}
'''
names=['bt_trim','bt_has_mac_pattern','bt_name_is_address','bt_extract_mac_from_line','bt_add_device_unique_ex','bt_ctl_run','bt_parse_ctl_devices','bt_get_device_name','bt_get_active_mac','bt_route_to_bluetooth']
tail=r'''
int main(void) {
    output="";exit_status=0;assert(bt_ctl_run("trust","AA:BB:CC:DD:EE:FF",NULL));
    assert(logs==1);
    assert(bt_ctl_run("power","on","power on succeeded"));
    assert(!bt_ctl_run("power","off","power on succeeded"));
    assert(!bt_ctl_run("power","hang","power on succeeded"));
    struct bt_device d[4]={0};
    output="Device AA:BB:CC:DD:EE:FF";assert(bt_parse_ctl_devices("devices",d,0,4,false)==1);
    assert(!strcmp(d[0].name,"AA:BB:CC:DD:EE:FF"));
    output="Device AA:BB:ZZ:DD:EE:FF Invalid\nDevice AA:BB:CC:DD:EE:FF Headphones\n";
    assert(bt_parse_ctl_devices("devices",d,0,4,true)==1);assert(d[0].paired);
    char name[BT_NAME_LEN];
    output="Name: Studio headphones\nAlias: AA:BB:CC:DD:EE:FF\n";
    bt_get_device_name("AA:BB:CC:DD:EE:FF",name,sizeof name);
    assert(!strcmp(name,"Studio headphones"));
    output="Name: Studio headphones\nAlias: My headphones\n";
    bt_get_device_name("AA:BB:CC:DD:EE:FF",name,sizeof name);
    assert(!strcmp(name,"My headphones"));
    output="Alias: AA-BB-CC-DD-EE-FF\n";name[0]=0;
    bt_get_device_name("AA:BB:CC:DD:EE:FF",name,sizeof name);
    assert(!name[0]);
    assert(bt_name_is_address("AA-BB-CC-DD-EE-FF")&&bt_name_is_address("aa:bb:cc:dd:ee:ff"));
    assert(!bt_name_is_address("Studio headphones"));
    char mac[18];assert(bt_extract_mac_from_line("/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink",mac,sizeof mac));
    assert(!strcmp(mac,"AA:BB:CC:DD:EE:FF"));
    output="/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink\n";
    assert(bt_get_active_mac(mac,sizeof mac));
    output="";assert(!bt_get_active_mac(mac,sizeof mac));
    strcpy(mac,"AA:BB:CC:DD:EE:FF");
    assert(bt_route_to_bluetooth(mac,NULL));assert(routed && !local && !bt_active_codec[0]);
    routed=false;ready=false;assert(!bt_route_to_bluetooth(mac,NULL));assert(local && !routed);
    puts("PASS: bounded Bluetooth commands, device parsing, MAC parsing, codec-independent routing, local fallback.");
}
'''
with tempfile.TemporaryDirectory(prefix='sloppy-bt-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(head+'\n'.join(function(n) for n in names)+tail)
    fake=p/'bluetoothctl'
    fake.write_text('#!/bin/sh\ncase "$1:$2" in\n  trust:*) exit 0;;\n  power:on) echo "Changing power on succeeded"; exit 0;;\n  power:hang) sleep 20;;\n  *) echo "Failed"; exit 1;;\nesac\n')
    fake.chmod(0o755)
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    env=os.environ.copy();env['PATH']=str(p)+os.pathsep+env.get('PATH','')
    subprocess.run([str(p/'test')],check=True,timeout=30,env=env)
