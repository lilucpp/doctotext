#!/usr/bin/env python3
import os
import shutil
import sys
import urllib.request

def main():
    target_dir = sys.argv[1] if len(sys.argv) > 1 else "3rdparty_backup"
    os.makedirs(target_dir, exist_ok=True)

    files = [
        'unzip101e.zip',
        'wv2-0.2.3_patched_4-private_headers.tar.bz2',
        'wv2-0.2.3_patched_4-private_headers.tar.bz2.sha1',
        'mimetic-0.9.7-fixed-i386-x86_64-macosx.tar.bz2',
        'mimetic-0.9.7-fixed-i386-x86_64-macosx.tar.bz2.sha1',
    ]

    arches = ['x86_64-linux', 'x86-linux', 'win64', 'win32']
    for arch in arches:
        files.extend([
            f'wv2-0.2.3_patched_4-{arch}.tar.bz2',
            f'wv2-0.2.3_patched_4-{arch}.tar.bz2.sha1',
            f'htmlcxx-0.85-{arch}.tar.bz2',
            f'htmlcxx-0.85-{arch}.tar.bz2.sha1',
            f'libcharsetdetect-1.0-{arch}.tar.bz2',
            f'libcharsetdetect-1.0-{arch}.tar.bz2.sha1',
        ])
        if arch == 'win32':
            files.extend([
                f'mimetic-0.9.7-{arch}-2.tar.bz2',
                f'mimetic-0.9.7-{arch}-2.tar.bz2.sha1',
            ])
        else:
            files.extend([
                f'mimetic-0.9.7-{arch}.tar.bz2',
                f'mimetic-0.9.7-{arch}.tar.bz2.sha1',
            ])
        if 'win' in arch:
            files.extend([
                f'pthreads-2.9.1-{arch}.tar.bz2',
                f'pthreads-2.9.1-{arch}.tar.bz2.sha1',
                f'libiconv-1.11-{arch}.tar.bz2',
                f'libiconv-1.11-{arch}.tar.bz2.sha1',
                f'zlib-1.2.7-{arch}.tar.bz2',
                f'zlib-1.2.7-{arch}.tar.bz2.sha1',
                f'libxml2-2.7.7-{arch}.tar.bz2',
                f'libxml2-2.7.7-{arch}.tar.bz2.sha1',
            ])

    base_url = 'http://silvercoders.com/download/3rdparty/'
    headers = {'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36'}

    # If local mac files exist in 3rdparty, copy them
    for mac_file in ['mimetic-0.9.7-fixed-i386-x86_64-macosx.tar.bz2', 'mimetic-0.9.7-fixed-i386-x86_64-macosx.tar.bz2.sha1']:
        local_src = os.path.join('3rdparty', mac_file)
        dest = os.path.join(target_dir, mac_file)
        if os.path.exists(local_src) and not os.path.exists(dest):
            shutil.copy2(local_src, dest)
            print(f'Copied from repo: {mac_file}')

    success = 0
    for fn in files:
        dest = os.path.join(target_dir, fn)
        if os.path.exists(dest) and os.path.getsize(dest) > 0:
            success += 1
            continue
        url = 'http://www.winimage.com/zLibDll/unzip101e.zip' if fn == 'unzip101e.zip' else base_url + fn
        try:
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=15) as resp, open(dest, 'wb') as out_f:
                out_f.write(resp.read())
            sz = os.path.getsize(dest)
            print(f'Downloaded {fn} ({sz} bytes)')
            success += 1
        except Exception as e:
            print(f'Failed {fn}: {e}')

    print(f'All dependencies ready: {success}/{len(files)} files in {target_dir}.')
    if success < len(files):
        sys.exit(1)

if __name__ == '__main__':
    main()
