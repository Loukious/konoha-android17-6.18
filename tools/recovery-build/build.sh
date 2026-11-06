#!/bin/bash
set -euo pipefail
# Run in the pinned toolchain container with /source read-only and /work writable.
export SOURCE_DATE_EPOCH=1793923200
export LC_ALL=C
python3 - <<'PY'
from pathlib import Path
import hashlib,json,tarfile
root=Path('/source/ci/recovery/vendor')
for record in json.loads((root/'sources.json').read_text()):
    source=root/record['archive']
    if hashlib.sha256(source.read_bytes()).hexdigest()!=record['sha256']:
        raise ValueError('Source checksum mismatch: '+str(source))
    # Only the hash-pinned source archives above are extracted in this container.
    with tarfile.open(source) as tar: tar.extractall('/work')
PY
prefix=/work/deps
mkdir -p "$prefix"
cd /work/musl-1.2.5
./configure --target=aarch64-linux-musl --prefix=/work/musl --disable-shared --enable-gcc-wrapper \
    CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc
make -j4
make install
for name in linux asm asm-generic; do
    cp -a "/usr/aarch64-linux-gnu/include/$name" /work/musl/include/
done
export CC=/work/musl/bin/musl-gcc
export AR=aarch64-linux-gnu-ar RANLIB=aarch64-linux-gnu-ranlib
export CFLAGS='-O2 -fno-ident -ffile-prefix-map=/work=.'
export LDFLAGS="-static -L$prefix/lib -Wl,--build-id=none"
export CPPFLAGS="-I$prefix/include"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
cd /work/lz4
make -C lib -j4 BUILD_SHARED=no PREFIX="$prefix" install
cd /work/zlib-1.3.1
CHOST=aarch64-linux-musl ./configure --static --prefix="$prefix"
make -j4
make install
cd /work/zstd
make -C lib -j4 ZSTD_LEGACY_SUPPORT=0 PREFIX="$prefix" libzstd.a
make -C lib ZSTD_LEGACY_SUPPORT=0 PREFIX="$prefix" install-includes install-pc
cp lib/libzstd.a "$prefix/lib/"
cd /work/erofs-utils
python3 - <<'PY'
from pathlib import Path
p=Path('mkfs/main.c')
s=p.read_text()
old='\t\t\tcfg.c_mt_workers = strtoul(optarg, &endptr, 0);'
assert s.count(old)==1
# strtoul need not clear an earlier errno on success (observable with musl).
s=s.replace(old,'\t\t\terrno = 0;\n'+old)
p.write_text(s)
p=Path('lib/super.c')
s=p.read_text()
old='\tmemcpy(&sbi->uuid, dsb->uuid, sizeof(dsb->uuid));'
assert s.count(old)==1
s=s.replace(old,old+'\n\tmemcpy(sbi->volume_name, dsb->volume_name, sizeof(dsb->volume_name));')
p.write_text(s)
p=Path('lib/tar.c')
s=p.read_text()
old='''\t\t\t\t\tret = sscanf(value + n + 1, "%d", &n);
\t\t\t\t\tif (ret < 1) {
\t\t\t\t\t\tret = -EIO;
\t\t\t\t\t\tgoto out;
\t\t\t\t\t}
\t\t\t\t\tST_MTIM_NSEC_SET(&eh->st, n);'''
new='''\t\t\t\t\tconst char *fraction = value + n + 1;
\t\t\t\t\tunsigned int nsec = 0, digits = 0;
\t\t\t\t\twhile (*fraction >= '0' && *fraction <= '9') {
\t\t\t\t\t\tif (digits < 9) {
\t\t\t\t\t\t\tnsec = nsec * 10 + (*fraction - '0');
\t\t\t\t\t\t\t++digits;
\t\t\t\t\t\t} else if (*fraction != '0') {
\t\t\t\t\t\t\tret = -EIO;
\t\t\t\t\t\t\tgoto out;
\t\t\t\t\t\t}
\t\t\t\t\t\t++fraction;
\t\t\t\t\t}
\t\t\t\t\tif (!digits || *fraction) {
\t\t\t\t\t\tret = -EIO;
\t\t\t\t\t\tgoto out;
\t\t\t\t\t}
\t\t\t\t\twhile (digits++ < 9) nsec *= 10;
\t\t\t\t\tST_MTIM_NSEC_SET(&eh->st, nsec);'''
assert s.count(old)==1
p.write_text(s.replace(old,new))
PY
autoreconf -fi
./configure --host=aarch64-linux-musl --disable-shared --enable-static --enable-multithreading \
    --disable-fuse --without-selinux --without-uuid --disable-lzma \
    --without-libdeflate --with-zlib --with-libzstd --enable-lz4
rm -f mkfs/mkfs.erofs fsck/fsck.erofs
make -j4 LDFLAGS="$LDFLAGS -all-static"
mkdir -p /work/output
cp mkfs/mkfs.erofs fsck/fsck.erofs /work/output/
"$CC" $CFLAGS $CPPFLAGS -I. -Iinclude /source/ci/recovery/erofs-meta.c \
    lib/.libs/liberofs.a $LDFLAGS -llz4 -lzstd -lz -lpthread -o /work/output/erofs-meta
for name in mkfs.erofs fsck.erofs erofs-meta; do
    tool=/work/output/$name
    if aarch64-linux-gnu-readelf -l "$tool" | grep -q INTERP; then
        echo "Tool must be static: $tool" >&2; exit 1
    fi
done
qemu-aarch64 /work/output/mkfs.erofs --version
qemu-aarch64 /work/output/fsck.erofs --version
cd /work/output
sha256sum mkfs.erofs fsck.erofs erofs-meta > SHA256SUMS
