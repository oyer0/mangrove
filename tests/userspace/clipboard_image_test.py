#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fresh and incremental clipd installation; preserve an occupied preferred ID."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path[:0] = [str(ROOT/'tools'), str(ROOT/'tests/filesystems/mgfs')]
import populate_mgfs as populate
import update_mgfs as update
from mgfs_update_record_reconcile_test import image_layout, checker_ok


def payload(image, layout, identifier):
    offset = update.record_offset(image, layout['record_table_start'],identifier,layout['record_count'])
    return update.record_payload(image,bytes(image[offset:offset+update.RECORD_BYTES]))


def main():
    mkmgfs, mgfsck = (Path(path).resolve() for path in sys.argv[1:])
    os.chdir(ROOT)
    storage = ROOT/'build/tests'
    storage.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='clipd-image-',dir=storage) as temporary:
        folder=Path(temporary);image_path=folder/'root.img';binary=folder/'payload'
        binary.write_bytes(b'clipboard-image-fixture'*64)
        paths=[str(binary)]*len(populate.PAYLOAD_MANIFEST)
        data=[str(binary)]*len(populate.DATA_MANIFEST)
        subprocess.run([str(mkmgfs),'--blocks','3600','--uuid',
            '11111111-2222-3333-4444-555555555555','--format-time-ns','1',str(image_path)],check=True,capture_output=True)
        subprocess.run([sys.executable,str(ROOT/'tools/populate_mgfs.py'),str(image_path),*paths,*data],check=True,capture_output=True)
        checker_ok(mgfsck,image_path)
        image=bytearray(image_path.read_bytes());layout=image_layout(image)
        assert update.directory_find(payload(image,layout,4),'clipd')==169
        assert update.directory_find(payload(image,layout,2),'clipd') is None
        # Simulate a pre-clipd image whose user data occupies the preferred
        # logical Record ID. Perform all changes offline through test tooling.
        core=update.directory_remove(payload(image,layout,4),'clipd')
        indices={update.replace_directory_payload(image,layout,4,core,set()),
                 update.release_record(image,layout,169)}
        slot=update.allocate_record_slots(image,layout,1)[0]
        expected=b'user-owned bytes must survive installation'
        indices.add(update._create_payload_record(image,layout,169,slot,1,expected,1,set(),owner_uid=1000,permissions=3))
        index=update.rewrite_record_security(image,layout,169,1000,3)
        if index is not None:indices.add(index)
        temp=payload(image,layout,6)+populate.directory_entry(169,'keep.txt')
        indices.add(update.replace_directory_payload(image,layout,6,temp,set()))
        for index in indices:update.refresh_table_checksum(image,layout,index)
        update.refresh_record_bitmap_checksum(image,layout)
        update.refresh_metadata_checksums(image,layout)
        image_path.write_bytes(image);checker_ok(mgfsck,image_path)
        assert update.update(str(image_path),paths,data)
        image=bytearray(image_path.read_bytes())
        installed=update.directory_find(payload(image,layout,4),'clipd')
        assert installed is not None and installed!=169
        assert payload(image,layout,installed)==binary.read_bytes()
        assert payload(image,layout,169)==expected
        assert update.directory_find(payload(image,layout,6),'keep.txt')==169
        assert update.directory_find(payload(image,layout,2),'clipd') is None
        checker_ok(mgfsck,image_path)
        previous=bytes(image)
        assert not update.update(str(image_path),paths,data)
        assert image_path.read_bytes()==previous
    print('clipd images: fresh core install, occupied-ID preservation, incremental install and idempotence passed')

if __name__=='__main__':main()
