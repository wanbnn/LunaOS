#!/usr/bin/env python3
"""Package a pinned Linux wireless source checkout; these sources are not guest drivers."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('checkout', type=Path)
args = parser.parse_args()
root = args.checkout.resolve()
commit = subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'], text=True).strip()
paths = [root/'COPYING'] + sorted((root/'LICENSES').rglob('*')) + sorted((root/'drivers/net/wireless').rglob('*'))
paths = [p for p in paths if p.is_file()]
output = Path('assets/driver-sources')
output.mkdir(parents=True, exist_ok=True)
archive = output/'linux-wireless.tar.gz'
with archive.open('wb') as raw:
    with gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0,compresslevel=9) as compressed:
        with tarfile.open(fileobj=compressed,mode='w|') as tar:
            for path in paths:
                data = path.read_bytes()
                info = tarfile.TarInfo('linux-wireless-src/'+path.relative_to(root).as_posix())
                info.size=len(data);info.mode=0o644;info.mtime=0
                tar.addfile(info,io.BytesIO(data))
manifest = {'repository':'https://github.com/torvalds/linux','commit':commit,
            'path':'drivers/net/wireless','source_only':True,'runtime_driver':False,
            'files':len(paths),'bytes':archive.stat().st_size,
            'sha256':hashlib.sha256(archive.read_bytes()).hexdigest()}
(output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
