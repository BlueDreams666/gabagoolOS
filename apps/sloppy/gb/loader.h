#ifndef __LOADER_H__
#define __LOADER_H__

int loader_init(const char *s, const char *save);
void cleanup(void);
int sram_save(void);
int sn_load(const char *path);
int sn_save(const char *path);

#endif
