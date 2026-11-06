#!/usr/bin/env python3
"""Read the actual release and configuration from a published MU300 kernel bundle."""
import gzip
import hashlib
from pathlib import Path
import re
import sys


def release_info(directory, key):
    image = (directory / 'Image').read_bytes()
    version = re.search(rb'Linux version ([0-9][^ \x00\n]+) ', image)
    if not version:
        raise ValueError('Image has no kernel version')
    krel = version[1].decode('ascii')
    if not re.fullmatch(re.escape(key) + r'\.\d+(?:-g[0-9a-f]+)?', krel):
        raise ValueError(f'Image is {krel}, not kernel {key}')
    recorded = directory / 'kernel.release'
    if recorded.exists() and recorded.read_text().strip() != krel:
        raise ValueError('kernel.release differs from Image')
    start = image.find(b'IKCFG_ST')
    end = image.find(b'IKCFG_ED', start + 8)
    if start < 0 or end < 0:
        raise ValueError('Image has no embedded configuration')
    config = gzip.decompress(image[start + 8:end]).decode()
    settings = sorted(line for line in config.splitlines() if line.startswith(('CONFIG_', '# CONFIG_')))
    if not settings:
        raise ValueError('Image has an empty configuration')
    digest = hashlib.sha256(('\n'.join(settings) + '\n').encode()).hexdigest()
    return krel, digest


if __name__ == '__main__':
    krel, digest = release_info(Path(sys.argv[1]), sys.argv[2])
    # kernel.org calls the initial 7.3.0 release's source tarball linux-7.3.tar.xz.
    kv = krel[:-2] if krel.endswith('.0') else krel
    print(f'KREL={krel}\nKV={kv}\nRELEASE_CONFIG={digest}')
