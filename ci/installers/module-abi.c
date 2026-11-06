/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only ELF64 module CRC checker, freestanding on Linux ARM64 and x86_64. */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;
#if defined(__aarch64__)
enum { OPEN=56, READ=63, WRITE=64, CLOSE=57, EXIT=93 };
static long call(long n,long a,long b,long c) {
 register long x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
 __asm__ volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory");return x0;
}
__asm__(".global _start\n_start:\nmov x0, sp\nbl entry\nbrk #0\n");
#elif defined(__x86_64__)
enum { OPEN=257, READ=0, WRITE=1, CLOSE=3, EXIT=60 };
static long call(long n,long a,long b,long c) {
 __asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return n;
}
__asm__(".global _start\n_start:\nmov %rsp,%rdi\nand $-16,%rsp\ncall entry\nud2\n");
#else
#error Unsupported host
#endif
static char abi[8*1024*1024];
static u8 elf[64*1024*1024];
static char *keys[131072];
static u32 values[131072];
static u64 len(const char *s){u64 n=0;while(s[n])n++;return n;}
static int eq(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static void say(const char *s){u64 n=len(s);while(n){long w=call(WRITE,2,(long)s,n);if(w<=0)return;s+=w;n-=w;}}
static long load(const char *path,void *buf,u64 cap){
 long fd=call(OPEN,-100,(long)path,0),total=0,n;if(fd<0)return -1;
 while((u64)total<cap && (n=call(READ,fd,(long)buf+total,cap-total))>0)total+=n;
 if(n<0){call(CLOSE,fd,0,0);return -1;}
 if((u64)total==cap){char more;if(call(READ,fd,(long)&more,1)!=0)total=-1;}
 call(CLOSE,fd,0,0);return total;
}
static u64 hash(const char *s){u32 h=2166136261U;while(*s)h=(h^(u8)*s++)*16777619U;return h&131071;}
static int put(char *s,u32 crc){u64 h=hash(s);for(u64 i=0;i<131072;i++,h=(h+1)&131071){if(!keys[h]){keys[h]=s;values[h]=crc;return 1;}if(eq(keys[h],s))return values[h]==crc;}return 0;}
static int check(const char *s,u32 crc){u64 h=hash(s);for(u64 i=0;i<131072&&keys[h];i++,h=(h+1)&131071)if(eq(keys[h],s))return values[h]==crc;say("Missing or mismatched ABI symbol: ");say(s);say("\n");return 0;}
static u64 number(const u8 *p,int n){u64 v=0;for(int i=n-1;i>=0;i--)v=(v<<8)|p[i];return v;}
static int bounds(u64 off,u64 n,u64 size){return off<=size&&n<=size-off;}
static int terminated(const u8 *p,u64 n){for(u64 i=0;i<n;i++)if(!p[i])return 1;return 0;}
static int module(const char *path){
 long size=load(path,elf,sizeof(elf));if(size<64)return 0;
 if(elf[0]!=127||elf[1]!='E'||elf[2]!='L'||elf[3]!='F'||elf[4]!=2||elf[5]!=1||number(elf+18,2)!=183)return 0;
 u64 sh=number(elf+40,8),ent=number(elf+58,2),count=number(elf+60,2),idx=number(elf+62,2);
 if(ent!=64||!count||idx>=count||!bounds(sh,count*ent,size))return 0;
 u8 *ns=elf+sh+idx*ent;u64 no=number(ns+24,8),nl=number(ns+32,8);
 if(!bounds(no,nl,size))return 0;
 u8 *classic=0,*names=0,*crcs=0;u64 cl=0,nlen=0,clen=0;
 for(u64 i=0;i<count;i++){
  u8 *h=elf+sh+i*ent;u64 name=number(h,4),off=number(h+24,8),n=number(h+32,8);
  if(name>=nl||!terminated(elf+no+name,nl-name))return 0;
  if(number(h+4,4)==8)continue;
  if(!bounds(off,n,size))return 0;
  char *s=(char*)elf+no+name;
  if(eq(s,"__versions")){if(classic)return 0;classic=elf+off;cl=n;}
  if(eq(s,"__version_ext_names")){if(names)return 0;names=elf+off;nlen=n;}
  if(eq(s,"__version_ext_crcs")){if(crcs)return 0;crcs=elf+off;clen=n;}
 }
 u64 checked=0;
 if(cl%64)return 0;
 for(u64 i=0;i<cl;i+=64){if(!terminated(classic+i+8,56)||!classic[i+8]||!check((char*)classic+i+8,number(classic+i,8)))return 0;checked++;}
 if(names||crcs){
  if(!names||!crcs||clen%4)return 0;u64 pos=0;
  for(u64 i=0;i<clen;i+=4){if(pos>=nlen||!names[pos]||!terminated(names+pos,nlen-pos)||!check((char*)names+pos,number(crcs+i,4)))return 0;pos+=len((char*)names+pos)+1;checked++;}
  while(pos<nlen)if(names[pos++])return 0;
 }
 return checked>0;
}
__attribute__((used)) void entry(long *stack){
 long argc=*stack;char **argv=(char**)(stack+1);int ok=argc>=3;
 long n=ok?load(argv[1],abi,sizeof(abi)-1):-1;ok=ok&&n>0;
 if(ok){abi[n]=0;char *p=abi;while(*p&&ok){u32 crc=0;int digits=0;
  while(*p && *p!=' '){int d=*p>='0'&&*p<='9'?*p-'0':*p>='a'&&*p<='f'?*p-'a'+10:-1;if(d<0||digits==8){ok=0;break;}crc=(crc<<4)|d;p++;digits++;}
  if(!ok||digits!=8||*p!=' '){ok=0;break;}char *key=++p;while(*p&&*p!='\n')p++;if(*p!='\n'||p==key){ok=0;break;}*p++=0;ok=put(key,crc);
 }}
 for(long i=2;i<argc&&ok;i++)if(!module(argv[i])){say("Module ABI check failed: ");say(argv[i]);say("\n");ok=0;}
 if(!ok)say("Kernel/module ABI mismatch or invalid input; nothing has been changed.\n");
 call(EXIT,ok?0:1,0,0);for(;;){}
}
