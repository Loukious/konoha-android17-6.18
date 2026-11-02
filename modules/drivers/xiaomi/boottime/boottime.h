#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>

#define BOOT_STR_SIZE 256
#define BUF_COUNT 12
#define LOGS_PER_BUF 80
#define MSG_SIZE 128

#ifdef CONFIG_BOOTPROF_THRESHOLD_MS
#define BOOTPROF_THRESHOLD (CONFIG_BOOTPROF_THRESHOLD_MS * 1000000)
#else
#define BOOTPROF_THRESHOLD 15000000
#endif

struct log_t {
	/* task cmdline for first 16 bytes
	 * and boot event for the rest
	 */
	char *comm_event;
	pid_t pid;
	u64 timestamp;
};

extern struct log_t *bootprof[BUF_COUNT];
extern unsigned long log_count;
unsigned long get_log_count(void);
struct log_t **get_bootprof_pointer(void);
bool mt_boot_finish(void);
void bootprof_log_boot(char *str);
