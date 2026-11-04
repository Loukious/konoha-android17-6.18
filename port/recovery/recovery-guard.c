/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only Linux recovery guard. No libc, interpreter, partition writes or
 * production test hooks. UAPI layouts follow Linux dm-ioctl.h/stat.h; snapshot
 * paths and clean protobuf semantics follow AOSP libsnapshot (Apache-2.0).
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed long long s64;
typedef unsigned long usize;

#if defined(__aarch64__)
enum {
  S_OPEN = 56,
  S_CLOSE = 57,
  S_READ = 63,
  S_WRITE = 64,
  S_DENTS = 61,
  S_STATX = 291,
  S_IOCTL = 29,
  S_FLOCK = 32,
  S_CLONE = 220,
  S_WAIT = 260,
  S_EXEC = 221,
  S_EXIT = 93
};
static long call(long n, long a, long b, long c, long d, long e, long f) {
  register long x8 __asm__("x8") = n, x0 __asm__("x0") = a,
                   x1 __asm__("x1") = b, x2 __asm__("x2") = c,
                   x3 __asm__("x3") = d, x4 __asm__("x4") = e,
                   x5 __asm__("x5") = f;
  __asm__ volatile("svc 0"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                   : "memory");
  return x0;
}
__asm__(".global _start\n_start:\nmov x0, sp\nbl entry\nbrk #0\n");
#elif defined(__x86_64__)
enum {
  S_OPEN = 257,
  S_CLOSE = 3,
  S_READ = 0,
  S_WRITE = 1,
  S_DENTS = 217,
  S_STATX = 332,
  S_IOCTL = 16,
  S_FLOCK = 73,
  S_CLONE = 56,
  S_WAIT = 61,
  S_EXEC = 59,
  S_EXIT = 60
};
static long call(long n, long a, long b, long c, long d, long e, long f) {
  register long r10 __asm__("r10") = d, r8 __asm__("r8") = e,
                    r9 __asm__("r9") = f;
  __asm__ volatile("syscall"
                   : "+a"(n)
                   : "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return n;
}
__asm__(".global _start\n_start:\nmov %rsp, %rdi\nand $-16, %rsp\ncall "
        "entry\nud2\n");
#else
#error Unsupported architecture
#endif
#define C3(n, a, b, c) call(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define AT_FDCWD -100
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000
#define O_NONBLOCK 0004000
#define AT_EMPTY_PATH 0x1000
#define AT_SYMLINK_NOFOLLOW 0x100
#define MODE_TYPE 0170000
#define MODE_DIR 0040000
#define MODE_REG 0100000
#define MODE_BLOCK 0060000
#define CAP 65536

static usize length(const char *p) {
  usize n = 0;
  while (p[n])
    n++;
  return n;
}
static int equal(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}
static void clear(void *v, usize n) {
  u8 *p = v;
  while (n--)
    *p++ = 0;
}
static void out(const char *p) {
  usize n = length(p);
  while (n) {
    long r = C3(S_WRITE, 1, p, n);
    if (r <= 0)
      return;
    p += r;
    n -= r;
  }
}
static void number(u64 n) {
  char b[24];
  usize i = 23;
  b[i] = 0;
  do {
    b[--i] = '0' + n % 10;
    n /= 10;
  } while (n);
  out(b + i);
}
static int fail(const char *p) {
  out("Guard refused: ");
  out(p);
  out("\n");
  return 1;
}
static long openat(int dir, const char *p, int flags) {
  return C3(S_OPEN, dir, p, flags);
}
static void closefd(int fd) { C3(S_CLOSE, fd, 0, 0); }
struct timestamp {
  s64 sec;
  u32 nsec;
  int reserved;
};
struct sx {
  u32 mask, blksize;
  u64 attributes;
  u32 nlink, uid, gid;
  u16 mode, pad;
  u64 ino, size, blocks, attrmask;
  struct timestamp times[4];
  u32 rdev_major, rdev_minor, dev_major, dev_minor;
  u8 spare[112];
};
_Static_assert(sizeof(struct sx) == 256, "statx ABI");
static int statx(int fd, const char *p, int flags, struct sx *s) {
  clear(s, sizeof(*s));
  long r = call(S_STATX, fd, (long)p, flags, 0x301, (long)s, 0);
  return r == 0 && (s->mask & 0x301) == 0x301;
}
static long readall(int fd, u8 *b, usize cap) {
  usize n = 0;
  while (n < cap) {
    long r = C3(S_READ, fd, b + n, cap - n);
    if (r == -4)
      continue;
    if (r < 0)
      return -1;
    if (!r)
      return n;
    n += r;
  }
  u8 extra;
  long r = C3(S_READ, fd, &extra, 1);
  return r == 0 ? (long)n : -1;
}
static int varint(const u8 *b, usize n, usize *p, u64 *v) {
  *v = 0;
  for (unsigned i = 0; i < 10; i++) {
    if (*p >= n)
      return 0;
    u8 c = b[(*p)++];
    if (i == 9 && c > 1)
      return 0;
    *v |= (u64)(c & 127) << (7 * i);
    if (!(c & 128))
      return i == 0 || (c & 127) != 0;
  }
  return 0;
}
/* WriteUpdateState(None) clears all fields. Permit known explicit defaults,
 * reject stale progress, duplicates, unknown fields and noncanonical encodings.
 * Exact legacy "none" is accepted; other legacy fallbacks are never accepted.
 */
static int clean_state(const u8 *b, usize n) {
  if (n == 4 && b[0] == 'n' && b[1] == 'o' && b[2] == 'n' && b[3] == 'e')
    return 1;
  usize p = 0;
  u32 seen = 0;
  while (p < n) {
    u64 key, v;
    if (!varint(b, n, &p, &key))
      return 0;
    u64 field = key >> 3, wire = key & 7;
    if (field < 1 || field > 18 || field == 11 || (seen & (1u << field)))
      return 0;
    seen |= 1u << field;
    if (field == 8) {
      if (wire != 2 || !varint(b, n, &p, &v) || v != 0)
        return 0;
    } else if (wire != 0 || !varint(b, n, &p, &v) || v != 0)
      return 0;
  }
  return 1;
}
static int snapshots(int dir) {
  long fd = openat(dir, "snapshots", O_DIRECTORY | O_NOFOLLOW);
  if (fd < 0)
    return fail("snapshot directory missing, unreadable or symlinked");
  static u8 entries[8192];
  for (;;) {
    long n = C3(S_DENTS, fd, entries, sizeof(entries));
    if (n == -4)
      continue;
    if (n < 0) {
      closefd(fd);
      return fail("snapshot enumeration failed");
    }
    if (!n)
      break;
    usize p = 0;
    while (p < (usize)n) {
      if ((usize)n - p < 20) {
        closefd(fd);
        return fail("invalid directory reply");
      }
      u16 len = *(u16 *)(entries + p + 16);
      if (len < 20 || len > (usize)n - p) {
        closefd(fd);
        return fail("invalid directory entry");
      }
      char *name = (char *)entries + p + 19;
      usize j = 0;
      while (j < (usize)len - 19 && name[j])
        j++;
      if (j == (usize)len - 19 || (!equal(name, ".") && !equal(name, ".."))) {
        closefd(fd);
        return fail("snapshot records or unknown entries remain");
      }
      p += len;
    }
  }
  closefd(fd);
  return 0;
}
static int snapshot_at(int dir) {
  long fd = openat(dir, "state", O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return fail("OTA state missing, unreadable or symlinked");
  struct sx s;
  static u8 b[4096];
  if (!statx(fd, "", AT_EMPTY_PATH, &s) || (s.mode & MODE_TYPE) != MODE_REG ||
      s.size > sizeof(b)) {
    closefd(fd);
    return fail("unsupported OTA state inode");
  }
  long n = readall(fd, b, sizeof(b));
  closefd(fd);
  if (n < 0 || (u64)n != s.size || !clean_state(b, n))
    return fail("OTA state active, corrupt or unknown");
  return snapshots(dir);
}
static int metadata_open(const char *path) {
  long fd = openat(AT_FDCWD, path, O_DIRECTORY | O_NOFOLLOW);
  if (fd < 0) {
    fail("cannot open OTA metadata directory");
    return -1;
  }
  return fd;
}
#ifdef GUARD_TEST
static int state_command(const char *path) {
  int fd = metadata_open(path);
  if (fd < 0)
    return 1;
  if (C3(S_FLOCK, fd, 1 | 4, 0) < 0) {
    closefd(fd);
    return fail("OTA metadata lock busy");
  }
  int result = snapshot_at(fd);
  closefd(fd);
  if (!result)
    out("Update state: none\nSnapshot records: none\n");
  return result;
}
#endif
struct dmio {
  u32 version[3], data_size, data_start, target_count;
  int open_count;
  u32 flags, event, padding;
  u64 dev;
  char name[128], uuid[129], data[7];
};
struct spec {
  u64 start, length;
  int status;
  u32 next;
  char type[16];
};
_Static_assert(sizeof(struct dmio) == 312, "dm ioctl ABI");
_Static_assert(sizeof(struct spec) == 40, "dm target ABI");
static int decimal(const char **p, u64 *v) {
  const char *s = *p;
  *v = 0;
  if (*s < '0' || *s > '9')
    return 0;
  do {
    u64 digit = *s - '0';
    if (*v > (~(u64)0 - digit) / 10)
      return 0;
    *v = *v * 10 + digit;
    s++;
  } while (*s >= '0' && *s <= '9');
  *p = s;
  return 1;
}
static int device(const char **p, u64 *major, u64 *minor) {
  return decimal(p, major) && *(*p)++ == ':' && decimal(p, minor);
}
static u32 dev_major(u64 d) { return (d >> 8) & 0xfff; }
static u32 dev_minor(u64 d) { return (d & 0xff) | ((d >> 12) & 0xfff00); }
static int unmounted(u32 major, u32 minor) {
  long fd = openat(AT_FDCWD, "/proc/self/mountinfo", 0);
  if (fd < 0)
    return fail("cannot inspect mount identities");
  static u8 b[CAP + 1];
  long n = readall(fd, b, CAP);
  closefd(fd);
  if (n < 0)
    return fail("mount list unreadable or too large");
  b[n] = 0;
  const char *p = (char *)b;
  while (*p) {
    for (int i = 0; i < 2; i++) {
      while (*p && *p != ' ' && *p != '\n')
        p++;
      if (*p != ' ')
        return fail("invalid mount list");
      p++;
    }
    u64 ma, mi;
    if (!device(&p, &ma, &mi) || *p != ' ')
      return fail("invalid mount device");
    if (ma == major && mi == minor)
      return fail("target device is mounted");
    while (*p && *p != '\n')
      p++;
    if (*p)
      p++;
  }
  return 0;
}
static int bounded_string(const char *s, usize n) {
  for (usize i = 0; i < n; i++)
    if (!s[i])
      return 1;
  return 0;
}
static int parse_table(u8 *buf, usize cap, const char *name, u32 ma, u32 mi,
                       u32 super_ma, u32 super_mi, u64 size, u64 super_size) {
  if (cap < sizeof(struct dmio))
    return fail("short DM reply");
  struct dmio *io = (struct dmio *)buf;
  if (io->version[0] != 4 || io->data_size > cap ||
      io->data_start != sizeof(*io) || io->data_size < io->data_start ||
      !io->target_count || io->target_count > 1024 ||
      (io->flags &
       ((1u << 1) | (1u << 6) | (1u << 8) | (1u << 12) | (1u << 17) | (1u << 18))) ||
      !(io->flags & (1u << 5)) || !(io->flags & (1u << 4)) ||
      !bounded_string(io->name, sizeof(io->name)) || !equal(io->name, name) ||
      !bounded_string(io->uuid, sizeof(io->uuid)) || dev_major(io->dev) != ma ||
      dev_minor(io->dev) != mi || !size || size % 512 || !super_size ||
      super_size % 512)
    return fail("unknown, suspended, inactive or truncated DM mapping");
  usize p = io->data_start;
  u64 sectors = 0;
  for (u32 i = 0; i < io->target_count; i++) {
    if (p > io->data_size || io->data_size - p <= sizeof(struct spec))
      return fail("missing DM target");
    struct spec *s = (struct spec *)(buf + p);
    usize next = io->data_start + (usize)s->next;
    if (p % 8 || next % 8 || next <= p + sizeof(*s) || next > cap ||
        (i + 1 < io->target_count && next > io->data_size) ||
        (i + 1 == io->target_count &&
         next != ((io->data_size + 7u) & ~(usize)7)) ||
        s->status || !bounded_string(s->type, sizeof(s->type)) ||
        !equal(s->type, "linear") || s->start != sectors || !s->length ||
        s->length > size / 512 - sectors)
      return fail("nonlinear, discontinuous or malformed DM target");
    usize end = next < io->data_size ? next : io->data_size;
    char *text = (char *)buf + p + sizeof(*s);
    if (!bounded_string(text, end - p - sizeof(*s)))
      return fail("unterminated DM parameters");
    const char *q = text;
    u64 backing_ma, backing_mi, offset;
    if (!device(&q, &backing_ma, &backing_mi) || *q++ != ' ' ||
        !decimal(&q, &offset) || *q || backing_ma != super_ma ||
        backing_mi != super_mi || offset > super_size / 512 ||
        s->length > super_size / 512 - offset)
      return fail("DM backing is not the physical super extent");
    sectors += s->length;
    p = next;
  }
  if (sectors != size / 512)
    return fail("DM extent size differs from partition");
  out("Mapping: ");
  out(name);
  out(" ");
  number(ma);
  out(":");
  number(mi);
  out(" event=");
  number(io->event);
  out(" uuid=");
  const char hex[] = "0123456789abcdef";
  for (usize i = 0; io->uuid[i]; i++) {
    char c[3] = {hex[(u8)io->uuid[i] >> 4], hex[(u8)io->uuid[i] & 15], 0};
    out(c);
  }
  out("\n");
  p = io->data_start;
  for (u32 i = 0; i < io->target_count; i++) {
    struct spec *s = (struct spec *)(buf + p);
    number(s->start);
    out(" ");
    number(s->length);
    out(" linear ");
    out((char *)buf + p + sizeof(*s));
    out("\n");
    p = io->data_start + s->next;
  }
  return 0;
}
static int block_open(const char *path, struct sx *s, u64 *size) {
  long fd = openat(AT_FDCWD, path, O_NONBLOCK);
  if (fd < 0)
    return -1;
  if (!statx(fd, "", AT_EMPTY_PATH, s) || (s->mode & MODE_TYPE) != MODE_BLOCK ||
      C3(S_IOCTL, fd, 0x80081272, size) < 0) {
    closefd(fd);
    return -1;
  }
  return fd;
}
static int block_command(const char *target) {
  struct sx s;
  u64 size = 0;
  int fd = block_open(target, &s, &size);
  if (fd < 0)
    return fail("cannot bind block identity");
  int result = unmounted(s.rdev_major, s.rdev_minor);
  if (!result) {
    out("Block: ");
    number(s.rdev_major);
    out(":");
    number(s.rdev_minor);
    out(" size=");
    number(size);
    out("\n");
  }
  closefd(fd);
  return result;
}
static int dm_command(const char *name, const char *target, const char *super) {
  if (!(equal(name, "system_dlkm_a") || equal(name, "system_dlkm_b") ||
        equal(name, "vendor_dlkm_a") || equal(name, "vendor_dlkm_b")))
    return fail("unexpected mapping name");
  struct sx t, s;
  u64 size = 0, super_size = 0;
  int tf = block_open(target, &t, &size),
      sf = block_open(super, &s, &super_size);
  if (tf < 0 || sf < 0) {
    if (tf >= 0)
      closefd(tf);
    if (sf >= 0)
      closefd(sf);
    return fail("cannot bind block identities");
  }
  if (t.rdev_major == s.rdev_major) {
    closefd(tf);
    closefd(sf);
    return fail("super is another device-mapper device");
  }
  if (unmounted(t.rdev_major, t.rdev_minor)) {
    closefd(tf);
    closefd(sf);
    return 1;
  }
  const char *controls[] = {"/dev/device-mapper", "/dev/block/mapper/control",
                            "/dev/mapper/control"};
  long fd = -1;
  for (usize i = 0; i < 3 && fd < 0; i++)
    fd = openat(AT_FDCWD, controls[i], 0);
  if (fd < 0) {
    closefd(tf);
    closefd(sf);
    return fail("device-mapper control unavailable");
  }
  static u8 buffer[CAP] __attribute__((aligned(8)));
  clear(buffer, sizeof(buffer));
  struct dmio *io = (struct dmio *)buffer;
  io->version[0] = 4;
  io->data_start = sizeof(*io);
  io->data_size = sizeof(buffer);
  io->flags = 1u << 4;
  for (usize i = 0; name[i]; i++)
    io->name[i] = name[i];
  long r = C3(S_IOCTL, fd, 0xc138fd0c, buffer);
  closefd(fd);
  int result = r < 0 ? fail("DM_TABLE_STATUS ioctl failed")
                     : parse_table(buffer, sizeof(buffer), name, t.rdev_major,
                                   t.rdev_minor, s.rdev_major, s.rdev_minor,
                                   size, super_size);
  closefd(tf);
  closefd(sf);
  return result;
}
static int run_command(const char *metadata, const char *busybox,
                       const char *script, char **env) {
  int fd = metadata_open(metadata);
  if (fd < 0)
    return 1;
  if (C3(S_FLOCK, fd, 2 | 4, 0) < 0) {
    closefd(fd);
    return fail("OTA metadata lock busy");
  }
  if (snapshot_at(fd)) {
    closefd(fd);
    return 1;
  }
  long child = call(S_CLONE, 17, 0, 0, 0, 0, 0);
  if (child < 0) {
    closefd(fd);
    return fail("cannot supervise installer");
  }
  if (!child) {
    char *args[] = {(char *)busybox, "sh", (char *)script, 0};
    C3(S_EXEC, busybox, args, env);
    C3(S_EXIT, 127, 0, 0);
  }
  int status = 0;
  long r;
  do {
    r = call(S_WAIT, child, (long)&status, 0, 0, 0, 0);
  } while (r == -4);
  closefd(fd);
  if (r != child || (status & 127))
    return fail("installer interrupted");
  return (status >> 8) & 255;
}
static int main(int argc, char **argv, char **env) {
#ifdef GUARD_TEST
  if (argc == 3 && equal(argv[1], "test-state"))
    return state_command(argv[2]);
  if (argc == 5 && equal(argv[1], "test-run"))
    return run_command(argv[2], argv[3], argv[4], env);
  if (argc == 3 && equal(argv[1], "test-proto")) {
    long fd = openat(AT_FDCWD, argv[2], 0);
    static u8 b[4096];
    if (fd < 0)
      return 1;
    long n = readall(fd, b, sizeof(b));
    closefd(fd);
    return n >= 0 && clean_state(b, n) ? 0 : 1;
  }
  if (argc == 3 && equal(argv[1], "test-table")) {
    long fd = openat(AT_FDCWD, argv[2], 0);
    static u8 b[CAP] __attribute__((aligned(8)));
    clear(b, sizeof(b));
    if (fd < 0)
      return 1;
    long n = readall(fd, b, sizeof(b));
    closefd(fd);
    return n < 0 ? 1
                 : parse_table(b, n, "vendor_dlkm_a", 253, 7, 8, 1, 4096,
                               1048576);
  }
#endif
  if (argc == 2 && equal(argv[1], "snapshot")) {
    int fd = metadata_open("/metadata/ota");
    if (fd < 0)
      return 1;
    int result = snapshot_at(fd);
    closefd(fd);
    return result;
  }
  if (argc == 5 && equal(argv[1], "dm"))
    return dm_command(argv[2], argv[3], argv[4]);
  if (argc == 3 && equal(argv[1], "block"))
    return block_command(argv[2]);
  if (argc == 4 && equal(argv[1], "run"))
    return run_command("/metadata/ota", argv[2], argv[3], env);
  return fail("expected snapshot, block TARGET, dm NAME TARGET SUPER, or run "
              "BUSYBOX INSTALLER");
}
__attribute__((used, noreturn)) void entry(usize *stack) {
  int argc = stack[0];
  char **argv = (char **)(stack + 1), **env = argv + argc + 1;
  int status = main(argc, argv, env);
  C3(S_EXIT, status, 0, 0);
  for (;;) {
  }
}
