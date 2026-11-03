#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef unsigned long long u64;
typedef unsigned long long phys_addr_t;
#define __packed __attribute__((packed))
#define __iomem
#define __user
#define U16_MAX UINT16_MAX
#define GFP_KERNEL 0
#define current get_current()
#define check_add_overflow(a,b,p) __builtin_add_overflow((a),(b),(p))
struct device { int unused; };
struct dentry { int unused; };
struct file { void *private_data; struct {struct dentry *dentry;}f_path; };
typedef u16 __le16;
typedef u32 __le32;
#define le32_to_cpu(v) (v)
struct scmi_protocol_handle;
struct scmi_xfer {struct {void *buf;size_t len;}rx;};
struct scmi_xfer_ops {
 int (*xfer_get_init)(const struct scmi_protocol_handle *,unsigned char,size_t,size_t,struct scmi_xfer **);
 int (*do_xfer)(const struct scmi_protocol_handle *,struct scmi_xfer *);
 void (*xfer_put)(const struct scmi_protocol_handle *,struct scmi_xfer *);
};
struct scmi_protocol_handle {const struct scmi_xfer_ops *xops;};
static int transfer_error, request_error, transfer_count, transfer_put_count;
static unsigned char response[16];
static size_t response_len=16;
static struct scmi_xfer xfer;
static int request(const struct scmi_protocol_handle *ph,unsigned char id,size_t tx,size_t rx,struct scmi_xfer **out) {assert(id==1 && !tx && rx==16);if(request_error)return request_error;*out=&xfer;xfer.rx.buf=response;xfer.rx.len=response_len;transfer_count++;return 0;}
static int transfer(const struct scmi_protocol_handle *ph,struct scmi_xfer *x) {assert(x==&xfer);return transfer_error;}
static void transfer_put(const struct scmi_protocol_handle *ph,struct scmi_xfer *x) {assert(x==&xfer);transfer_put_count++;}
/* INSERT_METADATA */
/* INSERT_DECLARATIONS */
static unsigned char wire[4096];
static size_t wire_len;
static int cases, reads, seq_mode, seq_reads, allocation_failure, allocation_call;
static int file_refs, file_error, fs_error, fs_calls;
static struct stats_info *published;
static void *devm_allocs[512];
static int devm_count, heap_count;
static void put32(size_t off,u32 value) { assert(off+4<=sizeof(wire));memcpy(wire+off,&value,4); }
static u32 readl(void *address) {
 unsigned char *ptr=address;assert(ptr>=wire && ptr+4<=wire+wire_len);reads++;
 if(ptr==wire+offsetof(struct scmi_stats,match_sequence)) {seq_reads++;if(seq_mode==1)return 1;if(seq_mode==2)return seq_reads*2;}
 u32 value;memcpy(&value,ptr,4);return value;
}
static bool fail_alloc(void) {return ++allocation_call==allocation_failure;}
static void *devm_kzalloc(struct device *dev,size_t n,int flags) {if(fail_alloc())return NULL;void *p=calloc(1,n);assert(p && devm_count<512);devm_allocs[devm_count++]=p;return p;}
static void *devm_kcalloc(struct device *dev,size_t n,size_t size,int flags) {return devm_kzalloc(dev,n*size,flags);}
static void *devm_ioremap(struct device *dev,u64 address,u32 size) {assert(address==0x81200000 && size==wire_len);return wire;}
static void *kcalloc(size_t n,size_t size,int flags) {if(fail_alloc())return NULL;void *p=calloc(n,size);assert(p);heap_count++;return p;}
static void *kzalloc(size_t size,int flags) {return kcalloc(1,size,flags);}
static void kfree(void *p) {if(p){assert(heap_count>0);heap_count--;free(p);}}
static int debugfs_file_get(struct dentry *dentry) {if(file_error)return file_error;file_refs++;return 0;}
static void debugfs_file_put(struct dentry *dentry) {assert(file_refs>0);file_refs--;}
static int scnprintf(char *out,size_t capacity,const char *format,...) {va_list ap;va_start(ap,format);int n=vsnprintf(out,capacity,format,ap);va_end(ap);return n<(int)capacity ? n : (capacity?(int)capacity-1:0);}
static ssize_t simple_read_from_buffer(char *to,size_t count,loff_t *pos,const void *from,size_t available) {if(*pos<0)return -EINVAL;if((size_t)*pos>=available)return 0;size_t length=available-*pos;if(length>count)length=count;memcpy(to,(char *)from+*pos,length);*pos+=length;return length;}
static int cpufreq_stats_create_fs(struct device *dev,struct stats_info *info) {fs_calls++;published=info;return fs_error;}
/* INSERT_FUNCTIONS */
#define CHECK(c) do {assert(c);cases++;}while(0)
static void reset(void) {while(devm_count)free(devm_allocs[--devm_count]);assert(!heap_count && !file_refs);memset(wire,0,sizeof(wire));reads=seq_mode=seq_reads=allocation_failure=allocation_call=file_error=fs_error=fs_calls=0;published=NULL;wire_len=128;put32(0,PERF_STATS_SIGNATURE);put32(4,1);put32(8,1);put32(12,0);put32(16,6);put32(24,2);put32(40,1000);put32(48,1);put32(52,2);put32(56,3);put32(60,4);put32(64,2000);put32(72,5);put32(76,6);put32(80,7);put32(84,8);}
static struct device device;
int main(void) {
 reset();CHECK(!cpufreq_stats_map(&device,0x81200000,wire_len) && fs_calls==1 && published);
 CHECK(published->domains[0].offset==24 && published->domains[0].num_levels==2 && published->domains[0].frequencies[1]==2000);
 u64 values[2];struct clkdom_entry *entry=&published->entries[0];CHECK(!stats_read_values(published,entry,values) && values[0]==((u64)2<<32|1) && values[1]==((u64)6<<32|5));
 entry=&published->entries[1];CHECK(!stats_read_values(published,entry,values) && values[0]==((u64)4<<32|3));
 entry->entry=ENTRY_MAX;CHECK(stats_read_values(published,entry,values)==-EINVAL);entry->entry=ENTRY_RESIDENCY;
 struct file file={.private_data=&published->entries[0]};char out[200]={0};loff_t pos=0;
 CHECK(cpufreq_stats_get(&file,out,sizeof(out),&pos)>0 && strstr(out,"1000 8589934593\n") && strstr(out,"2000 25769803781\n") && !file_refs && !heap_count);
 CHECK(cpufreq_stats_get(&file,out,sizeof(out),&pos)==0 && !file_refs);
 file.private_data=NULL;CHECK(cpufreq_stats_get(&file,out,sizeof(out),&pos)==-ENOENT && !file_refs);file.private_data=&published->entries[0];
 file_error=-ENOENT;CHECK(cpufreq_stats_get(&file,out,sizeof(out),&pos)==-ENOENT && !file_refs);file_error=0;
 allocation_call=0;allocation_failure=1;CHECK(cpufreq_stats_get(&file,out,sizeof(out),&pos)==-ENOMEM && !heap_count && !file_refs);
 allocation_call=0;allocation_failure=2;CHECK(cpufreq_stats_get(&file,out,sizeof(out),&pos)==-ENOMEM && !heap_count && !file_refs);allocation_failure=0;
 seq_mode=1;seq_reads=0;CHECK(stats_read_values(published,&published->entries[0],values)==-EAGAIN && seq_reads==PERF_STATS_ATTEMPTS);
 seq_mode=2;seq_reads=0;CHECK(stats_read_values(published,&published->entries[0],values)==-EAGAIN && seq_reads==2*PERF_STATS_ATTEMPTS);seq_mode=0;
 put32(24,1);CHECK(stats_read_values(published,&published->entries[0],values)==-ESTALE);put32(24,2);
 put32(40,1234);CHECK(stats_read_values(published,&published->entries[0],values)==-ESTALE);put32(40,1000);
 put32(16,UINT32_MAX);CHECK(stats_read_values(published,&published->entries[0],values)==-EPROTO);put32(16,6);
 CHECK(!stats_range_valid(published,UINT64_MAX,4) && !stats_range_valid(published,127,4) && stats_range_valid(published,128,0));
 reset();CHECK(cpufreq_stats_map(&device,0x81200000,0)==-ENODATA && !reads);
 CHECK(cpufreq_stats_map(&device,0x81200000,8)==-EPROTO && !reads);
 CHECK(cpufreq_stats_map(&device,UINT64_MAX,wire_len)==-ERANGE && !reads);
 reset();put32(0,0);CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-EPROTO && !fs_calls);
 reset();put32(4,2);CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-EOPNOTSUPP && !fs_calls);
 reset();put32(8,0);CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-EPROTO && !fs_calls);
 reset();put32(8,65535);CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-EPROTO && !fs_calls);
 reset();put32(16,0);CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-EPROTO && !fs_calls);
 reset();put32(16,UINT32_MAX);CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-EPROTO && !fs_calls);
 reset();put32(24,65535);CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-EPROTO && !fs_calls);
 reset();seq_mode=1;CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-EAGAIN && !fs_calls);
 reset();seq_mode=2;CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-EAGAIN && !fs_calls);
 for(int i=1;i<=4;i++){reset();allocation_failure=i;CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-ENOMEM && !fs_calls);}
 reset();fs_error=-ENOMEM;CHECK(cpufreq_stats_map(&device,0x81200000,wire_len)==-ENOMEM && fs_calls==1);
 reset();put32(24,0);CHECK(!cpufreq_stats_map(&device,0x81200000,wire_len));file.private_data=&published->entries[0];pos=0;CHECK(!cpufreq_stats_get(&file,out,sizeof(out),&pos) && !file_refs);
 reset();
#ifdef PERF_PROTOCOL_ATTRIBUTES
 struct scmi_xfer_ops xops={request,transfer,transfer_put};struct scmi_protocol_handle ph={.xops=&xops};u64 address=0;u32 size=0;
 CHECK(cpufreq_stats_perf_metadata(NULL,&address,&size)==-EOPNOTSUPP);
 request_error=-ENOMEM;CHECK(cpufreq_stats_perf_metadata(&ph,&address,&size)==-ENOMEM && !transfer_put_count);request_error=0;
 transfer_error=-EOPNOTSUPP;CHECK(cpufreq_stats_perf_metadata(&ph,&address,&size)==-EOPNOTSUPP && transfer_put_count==1);transfer_error=0;
 response_len=15;CHECK(cpufreq_stats_perf_metadata(&ph,&address,&size)==-EPROTO && transfer_put_count==2);response_len=16;
 CHECK(cpufreq_stats_perf_metadata(&ph,&address,&size)==-ENODATA && transfer_put_count==3);
 u32 low=0x81200000,high=1,length=128;memcpy(response+4,&low,4);memcpy(response+8,&high,4);memcpy(response+12,&length,4);
 CHECK(!cpufreq_stats_perf_metadata(&ph,&address,&size) && address==0x181200000ULL && size==128 && transfer_put_count==4);
#endif
 printf("%d\n",cases);return 0;
}
