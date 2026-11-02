/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t u8;
typedef uint64_t phys_addr_t;
struct device { int unused; };
struct qmi_handle { int unused; };
struct sockaddr_qrtr { int unused; };
struct qmi_txn { int unused; };
struct qmi_elem_info { int unused; };
struct qmi_response_type_v01 { uint16_t result, error; };
struct qcom_scm_vmperm { unsigned int vmid,perm; };
struct qcom_glink_mem_entry { int unused; };
#define QMI_RESULT_FAILURE_V01 1
#define QMI_RESULT_SUCCESS_V01 0
#define QMI_ERR_NO_MEMORY_V01 2
#define QMI_ERR_NONE_V01 0
#define QMI_ERR_INTERNAL_V01 3
#define QCOM_SCM_VMID_HLOS 3
#define VMID_MSS_MSA 15
#define PERM_READ 4
#define PERM_WRITE 2
#define PERM_EXEC 1
#define BIT_ULL(n) (UINT64_C(1) << (n))
#define GFP_KERNEL 0
#define min(a,b) ((a)<(b)?(a):(b))
#define check_add_overflow(a,b,out) __builtin_add_overflow(a,b,out)
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p)>UINTPTR_MAX-4095)
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
/* ACTUAL_HEADERS */
struct mutex { bool locked; };
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked=true; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked=false; }
struct memshare_driver { struct device *dev; struct mutex mem_share; };
struct memshare_child { struct device *dev; int client_id; unsigned int index; struct qcom_glink_mem_entry *mem_entry; };
static struct memshare_driver driver;
static struct memshare_driver *memsh_drv=&driver;
static struct memshare_child *memsh_child[MAX_CLIENTS];
static struct mem_blocks memblock[MAX_CLIENTS];
static uint32_t num_clients;
static unsigned long attrs;
struct qmi_elem_info mem_alloc_generic_resp_msg_data_v01_ei[1];
struct qmi_elem_info mem_free_generic_resp_msg_data_v01_ei[1];
static int allocation_failure, assign_failure, glink_failure, frees, allocs, mappings, glink_entries;
static unsigned int freed_size;
static struct mem_alloc_generic_resp_msg_v01 alloc_response;
static struct mem_free_generic_resp_msg_v01 free_response;
static void *dma_alloc_attrs(struct device *dev,unsigned int size,phys_addr_t *pa,int flags,unsigned long attr) {
 (void)dev;(void)flags;(void)attr; if(allocation_failure) return NULL;
 void *p=calloc(1,size);if(p){*pa=(uintptr_t)p;allocs++;}return p;
}
static void dma_free_attrs(struct device *dev,unsigned int size,void *p,phys_addr_t pa,unsigned long attr) {
 (void)dev;(void)pa;(void)attr; assert(mappings==0);free(p);frees++;allocs--;freed_size=size;
}
static int qcom_scm_assign_mem(phys_addr_t pa,unsigned int size,u64 *source,struct qcom_scm_vmperm *dest,unsigned int n) {
 (void)pa;assert(size>0);if(assign_failure)return -EIO;
 if(n==1 && dest[0].vmid==QCOM_SCM_VMID_HLOS){assert(*source & BIT_ULL(VMID_MSS_MSA));mappings--;}
 else{assert(*source==BIT_ULL(QCOM_SCM_VMID_HLOS));mappings++;}return 0;
}
static struct qcom_glink_mem_entry *qcom_glink_mem_entry_init(struct device *dev,void *p,phys_addr_t pa,unsigned int size,phys_addr_t addr) {
 (void)dev;(void)p;(void)pa;(void)size;(void)addr;if(glink_failure)return NULL;
 glink_entries++;return calloc(1,sizeof(struct qcom_glink_mem_entry));
}
static void qcom_glink_mem_entry_free(struct qcom_glink_mem_entry *p){free(p);glink_entries--;}
static int qmi_send_response(struct qmi_handle *h,struct sockaddr_qrtr *sq,struct qmi_txn *txn,int msg,int len,struct qmi_elem_info *ei,void *response) {
 (void)h;(void)sq;(void)txn;(void)len;(void)ei;
 if(msg==MEM_ALLOC_GENERIC_RESP_MSG_V01)alloc_response=*(struct mem_alloc_generic_resp_msg_v01 *)response;
 else free_response=*(struct mem_free_generic_resp_msg_v01 *)response;
 return 0;
}
/* ACTUAL_FUNCTIONS */
static void allocate(unsigned int bytes) {
 struct mem_alloc_generic_req_msg_v01 req={.client_id=0,.proc_id=DHMS_MEM_PROC_MPSS_V01,.num_bytes=bytes,.sequence_id=7};
 handle_alloc_generic_req(NULL,NULL,NULL,&req);
}
static void release(void) {
 struct mem_free_generic_req_msg_v01 req={.client_id=0,.proc_id=DHMS_MEM_PROC_MPSS_V01};
 handle_free_generic_req(NULL,NULL,NULL,&req);
}
int main(void) {
 struct device dev={};struct memshare_child child={.dev=&dev,.client_id=0};memsh_child[0]=&child;num_clients=1;
 memblock[0].client_id=0;memblock[0].peripheral=DHMS_MEM_PROC_MPSS_V01;
 memblock[0].init_size=8192;memblock[0].guard_band=1;
 memblock[0].hyp_map_info=(struct memshare_hyp_mapping){.num_vmids=1,.vmids={VMID_MSS_MSA},.perms={PERM_READ|PERM_WRITE}};
 assert(check_client(0,DHMS_MEM_PROC_MPSS_V01,0)==0);
 assert(check_client(7,DHMS_MEM_PROC_MPSS_V01,0)==DHMS_MEM_CLIENT_INVALID && num_clients==1);
 allocate(0);assert(alloc_response.resp.result==QMI_RESULT_FAILURE_V01 && !allocs);
 allocation_failure=1;allocate(8192);assert(!allocs && alloc_response.resp.result==1);allocation_failure=0;
 assign_failure=1;allocate(8192);assert(!allocs && !mappings && alloc_response.resp.result==1);assign_failure=0;
 allocate(20000);assert(alloc_response.resp.result==0 && alloc_response.dhms_mem_alloc_addr_info[0].num_bytes==8192);
 assert(allocs==1 && mappings==1 && memblock[0].alloc_size==12288);
 allocate(1024);assert(allocs==1 && mappings==1);
 assign_failure=1;int before=frees;release();assert(frees==before && memblock[0].allotted && free_response.resp.result==1);assign_failure=0;
 release();assert(!allocs && !mappings && !memblock[0].allotted && freed_size==12288 && free_response.resp.result==0);
 release();assert(!allocs && free_response.resp.result==0);
 memblock[0].guarantee=1;allocate(8192);release();assert(allocs==1 && free_response.resp.result==1);
 assert(memshare_reclaim(0)==0 && !allocs && memblock[0].guarantee);
 memblock[0].guarantee=0;memblock[0].hyp_map_info.num_vmids=2;
 memblock[0].hyp_map_info.vmids[1]=QCOM_SCM_VMID_HLOS;
 glink_failure=1;allocate(8192);assert(!allocs && !mappings && !glink_entries && alloc_response.resp.result==1);glink_failure=0;
 allocate(8192);allocate(8192);assert(glink_entries==1 && allocs==1);release();assert(!glink_entries && !allocs && !mappings);
 puts("{\"cases\":12,\"secure_reclaim_checked\":true,\"guard_size_checked\":true}");
}
