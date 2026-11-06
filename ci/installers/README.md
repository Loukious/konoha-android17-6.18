# Release installers

`ci/installer_assets.py` packages the exact verified `gki/Image` and
`platform/qca_cld3_wcn7750.ko` from a build bundle. Both ZIPs are recreated during
release validation; altered ZIP bytes are rejected even if the outer checksums
were regenerated. Archive ordering, modes and timestamps are deterministic.

AnyKernel3 core and its official `arm64-tools` branch are vendored separately.
`provenance.json` records both commits and archive hashes. The upstream license
is included in the AnyKernel ZIP. Onyx needs the ARM64 tools, not the 32-bit ARM
executables from upstream master.

The freestanding `module-abi.c` helper verifies classic and extended ELF symbol
CRCs. The ZIPs carry the helper source, compiler identity and build flags.
Rebuild with the recorded Android Clang using:

```python
import json, subprocess, hashlib
from pathlib import Path
root = Path('ci/installers')
p = json.loads((root / 'provenance.json').read_text())
subprocess.run(['clang', *p['compile_flags'], str(root / 'module-abi.c'),
                '-o', str(root / 'tools/module-abi')], check=True)
for key, name in [('module_abi_source_sha256', 'module-abi.c'),
                  ('module_abi_binary_sha256', 'tools/module-abi')]:
    p[key] = hashlib.sha256((root / name).read_bytes()).hexdigest()
(root / 'provenance.json').write_text(json.dumps(p, indent=2) + '\n')
```

Validation covers malformed ELF/CRC data, complete bundle round trips and
tampering, and WLAN dependency, refcount, unload failure, reload failure and
Wi-Fi state behavior. The ARM64 tools and ABI helper also passed read-only
execution on Onyx: the matching release WLAN module was accepted, the installed
older KMI module was rejected, and vendor-boot v4 modules were extracted from a
copied partition image. This does not claim a recovery flash or device boot test
of the generated release ZIPs.
