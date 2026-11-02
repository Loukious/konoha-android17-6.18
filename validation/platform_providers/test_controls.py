#!/usr/bin/env python3
"""Exercise actual ported C control paths with failure-injected provider stubs."""
import argparse, hashlib, json, os, re, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
M = Path(os.environ.get('KONOHA_MODULES_ROOT', ROOT / 'modules'))

def function(path, name):
    src = (M / path).read_text()
    match = re.search(r'(?m)^[^\n;{}]*\b' + name + r'\([^;{}]*\)\n\{', src)
    if not match:
        raise ValueError('Function not found: ' + name)
    end, depth = match.end(), 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[match.start():end]

BASE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <unistd.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef uint64_t u64;
#define __iomem
#define PAGE_SIZE 4096
#define GFP_KERNEL 0
#define pr_err(...) ((void)0)
#define mca_log_err(...) ((void)0)
#define mca_log_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define DMA_BAM_DBG(...) ((void)0)
#define unlikely(x) (x)
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x) >= (uintptr_t)-4095)
#define PTR_ERR(x) ((long)(intptr_t)(x))
#define kzalloc(n,f) calloc(1,n)
#define kcalloc(n,s,f) calloc(n,s)
#define kfree(p) free(p)
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
static void *kmemdup_nul(const void *p, size_t n, int flags) {
    char *b = malloc(n+1); if (b) {memcpy(b,p,n); b[n]=0;} return b;
}
static int kstrtou32(const char *s, unsigned base, u32 *out) {
    char *end; unsigned long long val; errno=0;
    if (!*s || *s=='-' || *s==' ' || *s=='\t') return -EINVAL;
    val=strtoull(s,&end,base);
    if (end==s || errno || val>UINT32_MAX) return -ERANGE;
    if (*end=='\n') ++end;
    if (*end) return -EINVAL;
    *out=val; return 0;
}
'''

fixtures = {}
fixtures['gear-input'] = BASE + function('include/soc/qcom/mpam.h', 'qcom_mpam_parse_gear') + r'''
int main(void) {
    const char *bad[]={"", "gear", "gear=", "prio=1", "gear=-1", "gear=4294967296",
        "gear=1,", ",gear=1", "gear=1,gear=2", "gear=1,prio=0", "gear=1x", "gear= 1", "gear=1\nfoo"};
    u32 value; unsigned tests=0;
    for (unsigned i=0;i<ARRAY_SIZE(bad);i++) {assert(qcom_mpam_parse_gear(bad[i],strlen(bad[i]),&value)<0);tests++;}
    const char *good[]={"gear=0","gear=1\n","gear=0x2","gear=4294967295"};
    u32 expected[]={0,1,2,UINT32_MAX};
    for (unsigned i=0;i<ARRAY_SIZE(good);i++) {assert(!qcom_mpam_parse_gear(good[i],strlen(good[i]),&value));assert(value==expected[i]);tests++;}
    char embedded[]={ 'g','e','a','r','=','1',0,'x' };
    assert(qcom_mpam_parse_gear(embedded,sizeof(embedded),&value)==-EINVAL);tests++;
    char longbuf[PAGE_SIZE];memset(longbuf,'a',sizeof(longbuf));
    assert(qcom_mpam_parse_gear(longbuf,sizeof(longbuf),&value)==-EINVAL);tests++;
    printf("%u\n",tests);
}
'''
hl = 'drivers/power/supply/mca/mca_hardware_ic/boost_ic/hl7603/'
hl_header=(M/(hl+'inc/hl7603.h')).read_text()
constants='\n'.join(x for x in hl_header.splitlines() if re.match(r'#define\s+(VOUT_REG|VOUT_REG_MAX|VOUT_REG_BASE|VOUT_REG_STEP)\b',x))
fixtures['hl7603-i2c-errors'] = BASE + constants + r'''
struct boost_bypass_dev {void *client;u32 vout_threshold;};
static int write_error, read_error, mismatch, expected, writes, reads;
static int i2c_smbus_write_byte_data(void *client,int reg,u8 value) {writes++;expected=value;return write_error;}
static int i2c_smbus_read_byte_data(void *client,int reg) {reads++;return read_error?read_error:expected+mismatch;}
''' + function(hl+'hl7603.c','hl7603_set_voltage_threshold_locked') + r'''
int main(void) {
    struct boost_bypass_dev info={0};unsigned tests=0;
    u32 bad[]={0,VOUT_REG_BASE-1,VOUT_REG_MAX+1,UINT32_MAX};
    for(unsigned i=0;i<ARRAY_SIZE(bad);i++){writes=reads=0;assert(hl7603_set_voltage_threshold_locked(&info,bad[i])==-EINVAL);assert(!writes&&!reads);tests++;}
    write_error=-EREMOTEIO;reads=0;assert(hl7603_set_voltage_threshold_locked(&info,VOUT_REG_BASE)==-EREMOTEIO);assert(!reads);tests++;
    write_error=0;read_error=-ENXIO;assert(hl7603_set_voltage_threshold_locked(&info,VOUT_REG_BASE)==-ENXIO);tests++;
    read_error=0;mismatch=1;assert(hl7603_set_voltage_threshold_locked(&info,VOUT_REG_BASE)==-EIO);tests++;
    mismatch=0;assert(!hl7603_set_voltage_threshold_locked(&info,VOUT_REG_BASE));assert(expected==0);tests++;
    assert(!hl7603_set_voltage_threshold_locked(&info,VOUT_REG_MAX));assert(expected==(VOUT_REG_MAX-VOUT_REG_BASE)/VOUT_REG_STEP);tests++;
    printf("%u\n",tests);
}
'''
fixtures['bam-remote-fifo-bounds'] = BASE + r'''
#define __iomem
#define MSM_SLIM_DESC_FIFO_SIZE 256
#define BAM_DESC_FIFO_SIZE 32768
struct resource {u64 start,end;};
struct bam_device {
    struct {bool is_r_mem;struct resource *r_res;void *r_vsbase;u32 r_pbase;} r_mem;
    void *dev,*ipc_log_dma;int active_channels;bool powered_remotely;
};
struct bam_chan {struct bam_device *bdev;unsigned id;void *fifo_virt;u64 fifo_phys;};
struct dma_chan {struct bam_chan *data;};
static struct bam_chan *to_bam_chan(struct dma_chan *c){return c->data;}
static size_t resource_size(struct resource *r){return r->end-r->start+1;}
#define memset_io memset
static void *dma_alloc_wc(void *d,size_t n,u64 *phys,int f){return calloc(1,n);}
static void bam_reset(struct bam_device *d){}
''' + function('drivers/dma/qcom/bam_dma.c','bam_alloc_chan') + r'''
int main(void) {
    unsigned char region[1024];memset(region,0xcc,sizeof(region));unsigned tests=0;
    struct resource res={.start=0x1000,.end=0x13ff};
    struct bam_device dev={.r_mem={.is_r_mem=true,.r_res=&res,.r_vsbase=region,.r_pbase=0x1000}};
    struct bam_chan a={.bdev=&dev,.id=2},b={.bdev=&dev,.id=0},c={.bdev=&dev,.id=4};
    struct dma_chan ca={&a},cb={&b},cc={&c};
    assert(!bam_alloc_chan(&ca));assert(a.fifo_virt==region+512&&a.fifo_phys==0x1200);tests++;
    assert(region[511]==0xcc&&region[768]==0xcc);tests++;
    assert(!bam_alloc_chan(&cb));assert(b.fifo_virt==region&&b.fifo_phys==0x1000);tests++;
    assert(res.start==0x1000&&res.end==0x13ff);tests++;
    assert(bam_alloc_chan(&cc)==-ENOSPC);assert(!c.fifo_virt);tests++;
    assert(!bam_alloc_chan(&ca));assert(dev.active_channels==2);tests++;
    memset(region+512,0xab,256);a.fifo_virt=NULL;a.fifo_phys=0;
    assert(!bam_alloc_chan(&ca));assert(a.fifo_virt==region+512&&region[512]==0);tests++;
    printf("%u\n",tests);
}
'''
fixtures['stats-ioctl-routing'] = BASE + r'''
struct sleep_stats {u32 stat_type,count;u64 last_entered_at,last_exited_at,accumulated;};
struct subsystem_data {const char *name;u32 smem_item,pid;};
static struct subsystem_data subsystems[]={
 {"modem",605,1},{"wpss",605,13},{"adsp",606,2},{"cdsp",607,5},{"cdsp1",607,12},
 {"gpdsp0",607,17},{"gpdsp1",607,18},{"slpi",608,3},{"gpu",609,0},{"display",610,0},
 {"adsp_island",613,2},{"slpi_island",613,3},{"apss",631,UINT32_MAX}};
struct stats_data {void *base;};
struct stats_config {size_t num_records;};
struct stats_drvdata {struct stats_config *config;struct stats_data *d;};
struct file {void *private_data;};
static int sleep_stats_mutex;
#define DDR_STATS_MAX_NUM_MODES 20
#define MODEM_IOCTL 1
#define WPSS_IOCTL 2
#define ADSP_IOCTL 3
#define CDSP_IOCTL 4
#define SLPI_IOCTL 5
#define GPU_IOCTL 6
#define DISPLAY_IOCTL 7
#define ADSP_ISLAND_IOCTL 8
#define SLPI_ISLAND_IOCTL 9
#define APSS_IOCTL 10
#define AOSD_IOCTL 11
#define CXSD_IOCTL 12
#define DDR_IOCTL 13
#define DDR_STATS_IOCTL 14
static u32 observed_host,observed_item;
static int ddr_reads,copy_error,smem_error;
static struct sleep_stats shared={.count=7};
static void *qcom_stats_smem_get(u32 host,u32 item,size_t min) {observed_host=host;observed_item=item;return smem_error?ERR_PTR(smem_error):&shared;}
static void qcom_stats_copy(struct sleep_stats *a,struct sleep_stats *b){*b=*a;}
static void qcom_stats_update_accumulated_duration(struct sleep_stats *s){}
static int qcom_stats_copy_to_user(unsigned long arg,struct sleep_stats *s,size_t n){return copy_error;}
#define memcpy_fromio memcpy
static int qcom_stats_ddr_freq_sync(int *modes,struct sleep_stats *s){ddr_reads++;*modes=0;return 0;}
''' + function('drivers/soc/qcom/qcom_stats.c','qcom_stats_device_ioctl_active') + r'''
int main(void) {
 struct stats_config config={0};struct stats_drvdata drv={.config=&config};struct file file={&drv};unsigned tests=0;
 const unsigned commands[]={MODEM_IOCTL,WPSS_IOCTL,ADSP_IOCTL,CDSP_IOCTL,SLPI_IOCTL,GPU_IOCTL,DISPLAY_IOCTL,ADSP_ISLAND_IOCTL,SLPI_ISLAND_IOCTL,APSS_IOCTL};
 const unsigned indices[]={0,1,2,3,7,8,9,10,11,12};
 for(unsigned i=0;i<ARRAY_SIZE(commands);i++){
   assert(!qcom_stats_device_ioctl_active(&file,commands[i],0));
   assert(observed_host==subsystems[indices[i]].pid&&observed_item==subsystems[indices[i]].smem_item);tests++;
 }
 const unsigned absent[]={AOSD_IOCTL,CXSD_IOCTL,DDR_IOCTL};
 for(unsigned i=0;i<ARRAY_SIZE(absent);i++){assert(qcom_stats_device_ioctl_active(&file,absent[i],0)==-EOPNOTSUPP);assert(!ddr_reads);tests++;}
 assert(qcom_stats_device_ioctl_active(&file,12345,0)==-EINVAL);assert(!ddr_reads);tests++;
 assert(!qcom_stats_device_ioctl_active(&file,DDR_STATS_IOCTL,0));assert(ddr_reads==1);tests++;
 copy_error=-EFAULT;assert(qcom_stats_device_ioctl_active(&file,MODEM_IOCTL,0)==-EFAULT);tests++;
 copy_error=0;smem_error=-EINVAL;assert(qcom_stats_device_ioctl_active(&file,MODEM_IOCTL,0)<0);tests++;
 printf("%u\n",tests);
}
'''

fixtures['pmic-response-validation'] = BASE + r"""
typedef int32_t s32;
#define MSG_OWNER_REG_DUMP 32783
#define MSG_TYPE_REQ_RESP 1
#define REG_DUMP_REG_READ_REQ 0x37
#define REG_DUMP_REG_WRITE_REQ 0x38
struct pmic_glink_hdr {u32 owner,type,opcode;};
struct reg_dump_read_resp_msg {struct pmic_glink_hdr hdr;u32 bus_id,pmic_sid,address,byte_count;u8 data[256];};
struct reg_dump_write_resp_msg {struct pmic_glink_hdr hdr;u32 return_status;};
struct pmic_glink_debug_dev {
 int response_lock,ack;bool pending,poisoned;u32 opcode,bus_id,sid,address,bytes;int response_error;
 struct reg_dump_read_resp_msg read_msg;
};
#define spin_lock_irqsave(lock,flags) ((void)(lock))
#define spin_unlock_irqrestore(lock,flags) ((void)(lock))
static void complete(int *ack){(*ack)++;}
""" + function('drivers/soc/qcom/pmic-glink-debug.c','pmic_glink_debug_callback') + r"""
static struct pmic_glink_debug_dev fresh(u32 opcode){
 struct pmic_glink_debug_dev gd={.pending=true,.opcode=opcode,.bus_id=2,.sid=3,.address=0x1234,.bytes=4};return gd;
}
int main(void) {
 unsigned tests=0;struct pmic_glink_debug_dev gd=fresh(REG_DUMP_REG_READ_REQ);
 struct reg_dump_read_resp_msg r={.hdr={MSG_OWNER_REG_DUMP,1,REG_DUMP_REG_READ_REQ},.bus_id=2,.pmic_sid=3,.address=0x1234,.byte_count=4,.data={1,2,3,4}};
 assert(pmic_glink_debug_callback(&gd,&r,0)==-EINVAL&&!gd.ack);tests++;
 r.hdr.owner=0;assert(pmic_glink_debug_callback(&gd,&r,sizeof(r))==-EINVAL&&!gd.ack);tests++;r.hdr.owner=MSG_OWNER_REG_DUMP;
 r.hdr.opcode=REG_DUMP_REG_WRITE_REQ;assert(!pmic_glink_debug_callback(&gd,&r,sizeof(r))&&!gd.ack);tests++;r.hdr.opcode=REG_DUMP_REG_READ_REQ;
 r.address++;assert(!pmic_glink_debug_callback(&gd,&r,sizeof(r))&&!gd.ack);tests++;r.address--;
 r.pmic_sid++;assert(!pmic_glink_debug_callback(&gd,&r,sizeof(r))&&!gd.ack);tests++;r.pmic_sid--;
 r.bus_id++;assert(!pmic_glink_debug_callback(&gd,&r,sizeof(r))&&!gd.ack);tests++;r.bus_id--;
 assert(!pmic_glink_debug_callback(&gd,&r,sizeof(r)));assert(gd.ack==1&&!gd.pending&&!memcmp(gd.read_msg.data,r.data,4));tests++;
 assert(!pmic_glink_debug_callback(&gd,&r,sizeof(r))&&gd.ack==1);tests++;
 gd=fresh(REG_DUMP_REG_READ_REQ);r.byte_count=257;assert(pmic_glink_debug_callback(&gd,&r,sizeof(r))==-EPROTO&&gd.poisoned&&gd.ack==1);tests++;
 gd=fresh(REG_DUMP_REG_READ_REQ);r.byte_count=3;assert(pmic_glink_debug_callback(&gd,&r,sizeof(r))==-EPROTO&&gd.poisoned);tests++;
 gd=fresh(REG_DUMP_REG_READ_REQ);r.byte_count=(u32)-EREMOTEIO;assert(pmic_glink_debug_callback(&gd,&r,sizeof(r))==-EREMOTEIO&&gd.ack==1);tests++;
 gd=fresh(REG_DUMP_REG_READ_REQ);r.byte_count=4;assert(pmic_glink_debug_callback(&gd,&r,sizeof(r)-1)==-EPROTO&&gd.poisoned);tests++;
 struct reg_dump_write_resp_msg w={.hdr={MSG_OWNER_REG_DUMP,1,REG_DUMP_REG_WRITE_REQ}};
 gd=fresh(REG_DUMP_REG_WRITE_REQ);assert(!pmic_glink_debug_callback(&gd,&w,sizeof(w))&&gd.ack==1);tests++;
 gd=fresh(REG_DUMP_REG_WRITE_REQ);w.return_status=1;assert(pmic_glink_debug_callback(&gd,&w,sizeof(w))==-EIO);tests++;
 gd=fresh(REG_DUMP_REG_WRITE_REQ);w.return_status=(u32)-EPERM;assert(pmic_glink_debug_callback(&gd,&w,sizeof(w))==-EPERM);tests++;
 printf("%u\n",tests);
}
"""
fixtures['cpu-partition-allocation'] = BASE + r"""
#define PARTID_AVAILABLE 48
#define MSC_MAX 4
struct config_item {int dummy;};
struct config_group {struct config_item cg_item;};
struct mpam_config_val {u32 cpbm,capacity,dspri,slc_partition_id;};
struct cpu_mpam_partition {struct config_group group;int part_id,monitor_id;struct mpam_config_val *val;};
static unsigned long bits;
static unsigned long *part_id_free_bitmap=&bits;
static struct mpam_config_val mpam_default_val={.cpbm=0xf,.capacity=100};
static bool can_monitor;
static int cpu_mpam_item_type,cpu_mpam_item_type_legacy,reset_error,allocations,fail_at;
static void *alloc(size_t n,size_t size){if (++allocations==fail_at)return NULL;return calloc(n,size);}
#undef kzalloc
#undef kcalloc
#define kzalloc(n,f) alloc(1,n)
#define kcalloc(n,s,f) alloc(n,s)
static int bitmap_find_next_zero_area(unsigned long *b,int size,int start,int count,int align){for(int i=start;i<size;i++)if(!(*b&(1UL<<i)))return i;return size;}
static void bitmap_set(unsigned long *b,int index,int count){*b|=1UL<<index;}
static void bitmap_clear(unsigned long *b,int index,int count){*b&=~(1UL<<index);}
static int cpu_mpam_reset_param(int part){return reset_error;}
static void config_group_init_type_name(struct config_group *group,const char *name,void *type){}
""" + function('drivers/soc/qcom/mpam/cpu_mpam.c','cpu_mpam_make_group_locked') + r"""
int main(void){
 unsigned tests=0;struct config_group *group;struct cpu_mpam_partition *part;
 bits=(1UL<<PARTID_AVAILABLE)-1;group=cpu_mpam_make_group_locked(NULL,"exhausted");assert(IS_ERR(group));assert(bits==(1UL<<PARTID_AVAILABLE)-1);tests++;
 bits=0;allocations=0;fail_at=1;group=cpu_mpam_make_group_locked(NULL,"oom");assert(IS_ERR(group)&&!bits);tests++;
 allocations=0;fail_at=2;group=cpu_mpam_make_group_locked(NULL,"oom-value");assert(IS_ERR(group)&&!bits);tests++;
 allocations=0;fail_at=0;reset_error=-EIO;group=cpu_mpam_make_group_locked(NULL,"firmware-error");assert(PTR_ERR(group)==-EIO&&bits==1);tests++;
 bits=0;reset_error=0;group=cpu_mpam_make_group_locked(NULL,"sparse-msc");assert(!IS_ERR(group));part=(struct cpu_mpam_partition *)group;
 assert(part->part_id==0&&part->monitor_id==INT_MAX&&bits==1);tests++;
 assert(part->val[3].cpbm==0xf&&part->val[3].capacity==100);tests++;
 free(part->val);free(part);
 printf("%u\n",tests);
}
"""

def main():
    p=argparse.ArgumentParser();p.add_argument('--report',type=Path);args=p.parse_args()
    report={'status':'PASS','fixtures':[],'source_sha256':{}}
    paths=['include/soc/qcom/mpam.h',hl+'hl7603.c','drivers/dma/qcom/bam_dma.c','drivers/soc/qcom/qcom_stats.c','drivers/soc/qcom/mpam/cpu_mpam.c','drivers/soc/qcom/pmic-glink-debug.c']
    report['source_sha256']={x:hashlib.sha256((M/x).read_bytes()).hexdigest() for x in paths}
    with tempfile.TemporaryDirectory(prefix='konoha-platform-host-') as temp:
        for name,source in fixtures.items():
            c=Path(temp)/(name+'.c');exe=c.with_suffix('');c.write_text(source)
            subprocess.run(['cc','-std=gnu11','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(c),'-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],text=True,capture_output=True,check=True)
            count=int(result.stdout.strip());report['fixtures'].append({'name':name,'cases':count,'sanitizers':'ASan+UBSan','status':'PASS'})
    report['cases']=sum(x['cases'] for x in report['fixtures'])
    if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
