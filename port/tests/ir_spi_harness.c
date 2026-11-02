/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#include <stdbool.h>
#define GFP_KERNEL 0
#define IR_SPI_BITS_PER_PULSE 16
#define IR_SPI_MAX_PERIODS 32768
#define DIV_ROUND_CLOSEST_ULL(n,d) (((n)+(d)/2)/(d))
#define GENMASK(h,l) ((uint32_t)((UINT32_MAX << (l)) & (UINT32_MAX >> (31-(h)))))
struct rc_dev { void *priv; };
struct spi_device { u32 max_speed_hz; int dev; };
struct regulator { int unused; };
struct spi_transfer { u32 speed_hz; unsigned int len; const void *tx_buf; };
struct ir_spi_data { u32 freq; bool negated; u16 pulse,space; struct rc_dev *rc;
 struct spi_device *spi; struct regulator *regulator; };
static int alloc_failure, enable_failure, spi_failure, allocated, enabled, disabled, transfers;
static unsigned int words, hz;
static u16 waveform[IR_SPI_MAX_PERIODS];
static void *kmalloc_array(size_t n, size_t size, int flags) {
 (void)flags; if (alloc_failure) return NULL; void *p=calloc(n,size); if(p) allocated++; return p;
}
static void kfree(void *p) { if(p) allocated--; free(p); }
static int regulator_enable(struct regulator *r) { (void)r; if(enable_failure) return -EIO; enabled++; return 0; }
static int regulator_disable(struct regulator *r) { (void)r; enabled--; disabled++; return 0; }
static int spi_sync_transfer(struct spi_device *s, struct spi_transfer *x, int n) {
 (void)s; assert(n==1); assert(x->len<=sizeof(waveform)); words=x->len/2; hz=x->speed_hz;
 memcpy(waveform,x->tx_buf,x->len); transfers++; return spi_failure ? -EIO : 0;
}
#define dev_err(...) ((void)0)
/* ACTUAL_FUNCTIONS */
int main(void) {
 struct spi_device spi={.max_speed_hz=1600000}; struct regulator reg={};
 struct ir_spi_data data={.spi=&spi,.regulator=&reg}; struct rc_dev rc={.priv=&data};
 unsigned int signal[]={1000,500,1000}, original[3]; memcpy(original,signal,sizeof(signal));
 assert(ir_spi_set_tx_carrier(&rc,0)==-EINVAL);
 assert(ir_spi_set_tx_carrier(&rc,100001)==-EINVAL);
 assert(ir_spi_set_tx_carrier(&rc,38000)==0);
 assert(ir_spi_set_duty_cycle(&rc,0)==-EINVAL);
 assert(ir_spi_set_duty_cycle(&rc,101)==-EINVAL);
 assert(ir_spi_set_duty_cycle(&rc,50)==0);
 assert(ir_spi_tx(&rc,signal,3)==3); assert(words==95 && hz==608000);
 for(unsigned int i=0;i<words;i++) assert(waveform[i]==((i>=38 && i<57)?data.space:data.pulse));
 assert(!memcmp(signal,original,sizeof(signal)) && !allocated && !enabled);
 data.negated=true; assert(ir_spi_set_duty_cycle(&rc,50)==0);
 assert(ir_spi_tx(&rc,signal,3)==3); assert(waveform[38]==0xffff && waveform[0]==data.pulse);
 alloc_failure=1; assert(ir_spi_tx(&rc,signal,3)==-ENOMEM); alloc_failure=0;
 enable_failure=1; assert(ir_spi_tx(&rc,signal,3)==-EIO); enable_failure=0;
 assert(!allocated && !enabled);
 spi_failure=1; assert(ir_spi_tx(&rc,signal,3)==-EIO); spi_failure=0;
 assert(!allocated && !enabled);
 unsigned int large[]={UINT32_MAX}; int before=transfers;
 assert(ir_spi_tx(&rc,large,1)==-EINVAL && transfers==before);
 assert(ir_spi_tx(&rc,signal,0)==-EINVAL);
 unsigned int tiny[]={1}; assert(ir_spi_tx(&rc,tiny,1)==-EINVAL);
 assert(ir_spi_set_tx_carrier(&rc,100000)==0);
 unsigned int exact[]={327680}; assert(ir_spi_tx(&rc,exact,1)==1 && words==32768);
 unsigned int over[]={327680,10}; assert(ir_spi_tx(&rc,over,2)==-EINVAL);
 assert(!allocated && !enabled && disabled==4);
 puts("{\"cases\":16,\"waveform_checked\":true,\"fault_cleanup_checked\":true}");
}
