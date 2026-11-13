#!/bin/bash
# Stage Apple's open source HFS for the ANGELICA built-in filesystem (bsd/angelica_hfs).
# dxb runs patches/*.sh from the XNU source root before applying *.patch.
set -euo pipefail

[[ "${MACHINE_CONFIG:-}" == ANGELICA ]] || exit 0
if [[ "${MACOS_VERSION:-}" == 11.5 ]]; then
    # hfs-556.100.11 is paired with xnu-7195.141.2 in Apple's macos-115
    # release.json (iOS 15 / macOS Big Sur 11.5). Fetch the immutable commit,
    # never a moving branch/tag.
    hfs_sha=ba1a9cdc6e83ae9dfb61e4b5a822fbf185ae5866
    hfs_ver='hfs-556.100.11'
elif [[ "${MACOS_VERSION:-}" == 13.5 ]]; then
    # hfs-627.100.6 is paired with xnu-8796.141.3 in Apple's macos-135
    # release.json. Fetch the immutable commit, never a moving branch/tag.
    hfs_sha=31d5ec238dfab485e78a9a30b8b92d13e8914f03
    hfs_ver='hfs-627.100.6'
else
    echo "ANGELICA HFS integration not configured for MACOS_VERSION=${MACOS_VERSION}" >&2
    exit 1
fi
[[ -f bsd/kern/bsd_init.c ]] || exit 1

hfs_tmp=$(mktemp -d)
trap 'rm -rf "$hfs_tmp"' EXIT
git init -q "$hfs_tmp"
git -C "$hfs_tmp" fetch -q --depth 1 \
    https://github.com/apple-oss-distributions/hfs "$hfs_sha"
git -C "$hfs_tmp" checkout -q FETCH_HEAD
[[ "$(git -C "$hfs_tmp" rev-parse HEAD)" == "$hfs_sha" ]]

python3 - "$hfs_tmp" <<'PY'
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
target = Path('bsd/angelica_hfs')
if target.exists():
    raise SystemExit(f'refusing to overwrite existing HFS sources: {target}')
# Preserve upstream copyright/license headers; no downloaded sources in git.
for directory in ('core', 'hfs_encodings'):
    shutil.copytree(source / directory, target / directory)

# ANGELICA has no AppleKeyStore (content protection is not built, see
# the ANGELICA HFS compat header); its header is only needed for the key-wrapping API.
path = target / 'core' / 'hfs_iokit.h'
text = path.read_text()
include = '#include <AppleKeyStore/AppleKeyStoreFSServices.h>\n'
first = 'int hfs_unwrap_key('
end = '__END_DECLS'
if text.count(include) != 1 or text.count(first) != 1 or text.count(end) != 1:
    raise SystemExit(f'HFS source changed: {path}')
text = text.replace(include, '#if CONFIG_PROTECT\n' + include + '#endif\n')
i, j = text.index(first), text.index(end)
text = text[:i] + '#if CONFIG_PROTECT\n' + text[i:j].rstrip('\n') + '\n#endif\n\n' + text[j:]
path.write_text(text)
PY

# Kernel builds use -nostdinc; HFS's kext-config.h needs the SDK's target macros.
hfs_sdk=$(xcrun --sdk macosx --show-sdk-path)
cp "$hfs_sdk/usr/include/TargetConditionals.h" bsd/angelica_hfs/
echo "Staged $hfs_ver ($hfs_sha) for ANGELICA"
