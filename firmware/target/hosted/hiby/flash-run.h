/* The player started from the R1's own flash, not from RAM (flash-run.c). */
#ifndef FLASH_RUN_H
#define FLASH_RUN_H
#define FLASH_RUN_SRC   "/tmp/rockbox.r1"            /* where the boot menu puts and starts it */
#define FLASH_RUN_DIR   "/usr/data/gabagool"
#define FLASH_RUN_FILE  "rockbox.r1"
enum { FLASH_RUN_OFF, FLASH_RUN_FLASH, FLASH_RUN_NO_COPY, FLASH_RUN_GAVE_UP, FLASH_RUN_BAD, FLASH_RUN_EXEC };
void flash_run_boot(const char *src, const char *dir);
void flash_run_ok(const char *src, const char *dir);
int flash_run_copy(const char *src, const char *dir);
int flash_run_state(void);
const char *flash_run_says(void);
#endif
