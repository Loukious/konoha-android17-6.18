#!/bin/sh
# Build without a sysroot or runtime libraries. CLANG may name a local prebuilt.
set -eu
cd "$(dirname "$0")"
mkdir -p tools
KONOHA_GUARD_CLANG=${CLANG:-clang}
"$KONOHA_GUARD_CLANG" --target=aarch64-linux-gnu -fuse-ld=lld -nostdlib -static \
  -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables \
  -fno-asynchronous-unwind-tables -fno-ident -Os -Wall -Wextra -Werror \
  -Wl,--build-id=none -Wl,-z,noexecstack recovery-guard.c -o tools/recovery-guard
export KONOHA_GUARD_CLANG
python3 - <<'PY'
import hashlib, json, os, shutil, subprocess
from pathlib import Path
def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()
compiler = Path(shutil.which(os.environ['KONOHA_GUARD_CLANG'])).resolve()
receipt = {
    'source_sha256': sha('recovery-guard.c'),
    'binary_sha256': sha('tools/recovery-guard'),
    'compiler_sha256': sha(compiler),
    'compiler_version': subprocess.check_output([str(compiler), '--version'], text=True).splitlines()[0],
    'command': ['clang', '--target=aarch64-linux-gnu', '-fuse-ld=lld', '-nostdlib', '-static',
                '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables',
                '-fno-asynchronous-unwind-tables', '-fno-ident', '-Os', '-Wall', '-Wextra', '-Werror',
                '-Wl,--build-id=none', '-Wl,-z,noexecstack', 'recovery-guard.c', '-o', 'tools/recovery-guard'],
    'limits': 'Offline cross-build; static ELF inspection and host-native tests do not establish recovery runtime behavior.'
}
Path('tools/recovery-guard-build.json').write_text(json.dumps(receipt, indent=2) + '\n')
PY
