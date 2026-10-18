"""Builds TRP's RTX 40 copy of NVIDIA's NR runtime 310.9.16. Run with python -I.

NVIDIA ships this runtime with kernels for RTX 50 (sm_120) only, as PTX plus
native code. This script compiles NVIDIA's own PTX for Ada (sm_89) with the
CUDA toolkit, lowering the few sm_90+ features in four modules (ada_lowering.py),
and adds those kernels next to NVIDIA's untouched sm_120 code and PTX.

Host changes are limited to what loading them needs, and each patch site must
hold NVIDIA's original bytes first, so any other input fails instead of
producing a broken DLL:
- the NR feature-requirements minimum and CreateFeature's architecture switch
  admit Ada; Turing, Ampere and Hopper stay refused with NVIDIA's own error;
- the 7 network and 8 helper kernel bundles point at new bundles in an
  appended read-only section, with exact lengths.
The result is unsigned: NVIDIA's certificate is removed because it no longer
matches. Usage:
    python -I build_rtx40.py --official nvngx_dlssnr.dll --cuda <CUDA root> --output <dll>
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ada_lowering

OFFICIAL_SHA256 = '2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F'
FATBIN_MAGIC = b'\x50\xed\x55\xba\x01\x00\x10\x00'
IMAGE_BASE = 0x180000000
SECTION_NAME = b'.trpnr'

# Kernel bundles: (name, how the runtime references it, length variable).
# 'lea' sites are RIP-relative `lea reg, [bundle]`; 'pointer' sites are 64-bit
# absolute pointers in .data that the existing relocation table already covers.
BUNDLES = [
    ('network 0 swin 1h', ('lea', 0x605F3), 0x470540, 0x1106D0),
    ('network 1 swin 2h', ('lea', 0x6060B), 0x470548, 0x1DDED0),
    ('network 2 swin 4h', ('lea', 0x60626), 0x470550, 0x265EC0),
    ('network 3 swin 8h', ('lea', 0x60641), 0x470558, 0x2E1920),
    ('network 4 split swin 16h', ('lea', 0x6065C), 0x470560, 0x370D70),
    ('network 5 vit', ('lea', 0x60677), 0x470568, 0x3DA990),
    ('network 6 decoder upsample', ('lea', 0x60692), 0x470570, 0x45F280),
    ('helper font', ('lea', 0x6D528), 0x473090, 0x471140),
    ('helper capture', ('lea', 0x6D53B), 0x475B38, 0x4730A0),
    ('helper exposure scale', ('lea', 0x6D54F), 0x478950, 0x475B40),
    ('helper motion dilate', ('lea', 0x6D563), 0x47B2A0, 0x478960),
    ('helper buffer as texture', ('lea', 0x6D577), 0x47D998, 0x47B2B0),
    ('helper clear view', ('lea', 0x6D58B), 0x47F338, 0x47D9A0),
    ('helper post process', ('pointer', 0x1100C8), 0x48C968, 0x47F340),
    ('helper copy', ('pointer', 0x110100), 0x48E488, 0x48C970),
]
# Bundle index -> ada_lowering module number (NVIDIA's file order).
LOWERED = {4: 4, 5: 5, 6: 6, 13: 13}
# `mov dword [rsp+0x3C], minimum architecture` in the D3D11/D3D12 requirement exports.
REQUIREMENT_SITES = (0x136E0, 0x15580)
REQUIREMENT_ORIGINAL = bytes.fromhex('c744243cb0010000')  # 0x1B0, Blackwell
ADA = 0x190  # NV_GPU_ARCHITECTURE_AD100
# CreateFeature's jump-table entry for architecture 0x190: the rejection block,
# or the continuation used for supported architectures.
JUMP_ENTRY, JUMP_REJECT, JUMP_CONTINUE = 0x18150, 0x17BDA, 0x17C2B


class Image:
    def __init__(self, data):
        self.data = bytearray(data)
        self.pe = struct.unpack_from('<I', data, 0x3C)[0]
        self.optional = self.pe + 24
        self.count = struct.unpack_from('<H', data, self.pe + 6)[0]
        size = struct.unpack_from('<H', data, self.pe + 20)[0]
        assert struct.unpack_from('<H', data, self.optional)[0] == 0x20B, 'expected PE32+'
        self.table = self.optional + size
        self.section_align, self.file_align = struct.unpack_from('<II', data, self.optional + 32)
        assert struct.unpack_from('<Q', data, self.optional + 24)[0] == IMAGE_BASE

    def sections(self):
        for i in range(self.count):
            o = self.table + 40 * i
            name, vsize, va, rawsize, rawptr = struct.unpack_from('<8sIIII', self.data, o)
            yield dict(name=name.rstrip(b'\0'), header=o, vsize=vsize, va=va, rawsize=rawsize, rawptr=rawptr)

    def offset(self, rva):
        for s in self.sections():
            if s['va'] <= rva < s['va'] + max(s['vsize'], s['rawsize']):
                return s['rawptr'] + rva - s['va']
        raise ValueError(f'RVA 0x{rva:X} is not in a section')

    def read(self, rva, size):
        o = self.offset(rva)
        return bytes(self.data[o:o + size])

    def write(self, rva, value, expected):
        o = self.offset(rva)
        assert self.data[o:o + len(value)] == expected, f'unexpected bytes at RVA 0x{rva:X}'
        self.data[o:o + len(value)] = value


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def fatbin_at(image, rva):
    header = image.read(rva, 16)
    assert header[:8] == FATBIN_MAGIC, f'no fatbin at RVA 0x{rva:X}'
    return image.read(rva, 16 + struct.unpack_from('<Q', header, 8)[0])


def tool(cuda, name, *args):
    return subprocess.run([str(Path(cuda) / 'bin' / name), *map(str, args)], check=True, capture_output=True, text=True).stdout


def build_bundle(cuda, index, original, work):
    """NVIDIA's sm_120 cubin and PTX, plus an sm_89 cubin compiled from that PTX."""
    folder = work / f'bundle{index:02}'
    folder.mkdir()
    source = folder / 'nvidia.fatbin'
    source.write_bytes(original)
    subprocess.run([str(Path(cuda) / 'bin' / 'cuobjdump'), '-xelf', 'all', source.name], cwd=folder, check=True, capture_output=True)
    subprocess.run([str(Path(cuda) / 'bin' / 'cuobjdump'), '-xptx', 'all', source.name], cwd=folder, check=True, capture_output=True)
    cubins = sorted(folder.glob('*.sm_120.cubin'))
    ptxs = sorted(folder.glob('*.sm_120.ptx'))
    assert len(cubins) == 1 and len(ptxs) == 1, f'bundle {index}: expected one sm_120 cubin and PTX'
    ptx = ptxs[0].read_bytes().decode('ascii').replace('\r', '')
    if index in LOWERED:
        ada, rewrites, _, _ = ada_lowering.lower(ptx, LOWERED[index])
    else:
        assert ptx.count('.target sm_120') == 1, f'bundle {index}: unexpected target'
        ada, rewrites = ptx.replace('.target sm_120', '.target sm_89'), {}
    ada_ptx = folder / 'ada.ptx'
    ada_ptx.write_text(ada, encoding='ascii', newline='\n')
    ada_cubin = folder / 'ada.sm_89.cubin'
    tool(cuda, 'ptxas', '-arch=sm_89', '-O3', ada_ptx, '-o', ada_cubin)
    bundle = folder / 'trp.fatbin'
    tool(cuda, 'fatbinary', '--64', '--compress-all', f'--create={bundle}',
         f'--image3=kind=elf,sm=89,file={ada_cubin}',
         f'--image3=kind=elf,sm=120,file={cubins[0]}',
         f'--image3=kind=ptx,sm=120,file={ptxs[0]}')
    payload = bundle.read_bytes()
    assert payload[:8] == FATBIN_MAGIC
    payload = payload[:16 + struct.unpack_from('<Q', payload, 8)[0]]
    # The new bundle must decode back to exactly NVIDIA's sm_120 code and our sm_89 code.
    check = folder / 'check'
    check.mkdir()
    (check / 'trp.fatbin').write_bytes(payload)
    subprocess.run([str(Path(cuda) / 'bin' / 'cuobjdump'), '-xelf', 'all', 'trp.fatbin'], cwd=check, check=True, capture_output=True)
    extracted = {p.name.split('.')[-2]: p.read_bytes() for p in check.glob('*.cubin')}
    assert extracted.get('sm_120') == cubins[0].read_bytes(), f'bundle {index}: sm_120 code changed'
    assert extracted.get('sm_89') == ada_cubin.read_bytes(), f'bundle {index}: sm_89 code changed'
    sha = lambda b: hashlib.sha256(b).hexdigest().upper()
    return payload, dict(entries=re.findall(r'\.entry\s+(\w+)', ptx), rewrites=rewrites,
                         nvidiaSm120=sha(cubins[0].read_bytes()), nvidiaPtx=sha(ptxs[0].read_bytes()),
                         trpSm89=sha(ada_cubin.read_bytes()), bundle=sha(payload), bundleBytes=len(payload))


def checksum(data, field):
    total = 0
    padded = bytes(data) + b'\0' * (len(data) % 2)
    for (word,) in struct.iter_unpack('<H', padded):
        total += word
        total = (total & 0xFFFF) + (total >> 16)
    for i in range(field, field + 4, 2):  # the checksum field itself counts as zero
        total -= struct.unpack_from('<H', padded, i)[0]
        if total < 0:
            total += 0xFFFF
    total = (total & 0xFFFF) + (total >> 16)
    return ((total + (total >> 16)) & 0xFFFF) + len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--official', type=Path, required=True)
    parser.add_argument('--cuda', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = args.official.read_bytes()
    assert hashlib.sha256(data).hexdigest().upper() == OFFICIAL_SHA256, 'not NVIDIA NR 310.9.16'
    toolchain = tool(args.cuda, 'ptxas', '--version').strip().splitlines()[-1]
    image = Image(data)
    sections = list(image.sections())
    last = sections[-1]

    # Drop NVIDIA's certificate, which follows the last section and no longer applies.
    security = image.optional + 112 + 8 * 4
    cert_offset, cert_size = struct.unpack_from('<II', image.data, security)
    assert cert_offset == last['rawptr'] + last['rawsize'] and cert_offset + cert_size == len(image.data)
    del image.data[cert_offset:]
    struct.pack_into('<II', image.data, security, 0, 0)

    report = dict(official=OFFICIAL_SHA256, toolchain=toolchain, bundles=[])
    blob = bytearray()
    va = align(last['va'] + last['vsize'], image.section_align)
    with tempfile.TemporaryDirectory() as temporary:
        for index, (name, (kind, site), length, original_rva) in enumerate(BUNDLES):
            original = fatbin_at(image, original_rva)
            assert struct.unpack_from('<Q', image.read(length, 8))[0] == len(original), f'{name}: length differs'
            payload, details = build_bundle(args.cuda, index, original, Path(temporary))
            blob += b'\0' * (align(len(blob), 256) - len(blob))
            rva = va + len(blob)
            blob += payload
            if kind == 'lea':
                code = image.read(site, 7)
                assert code[0] in (0x48, 0x4C) and code[1] == 0x8D and code[2] & 0xC7 == 0x05, f'{name}: not lea'
                assert site + 7 + struct.unpack_from('<i', code, 3)[0] == original_rva, f'{name}: lea target differs'
                image.write(site + 3, struct.pack('<i', rva - (site + 7)), code[3:])
            else:
                image.write(site, struct.pack('<Q', IMAGE_BASE + rva), struct.pack('<Q', IMAGE_BASE + original_rva))
            image.write(length, struct.pack('<Q', len(payload)), struct.pack('<Q', len(original)))
            details.update(name=name, rva=f'0x{rva:X}')
            report['bundles'].append(details)
            print(f'{name}: {len(details["entries"])} kernels, {len(payload)} bytes')

    for site in REQUIREMENT_SITES:
        image.write(site + 4, struct.pack('<I', ADA), REQUIREMENT_ORIGINAL[4:])
    image.write(JUMP_ENTRY, struct.pack('<I', JUMP_CONTINUE), struct.pack('<I', JUMP_REJECT))

    # Append the read-only section that holds the new bundles.
    header = image.table + 40 * image.count
    assert header + 40 <= min(s['rawptr'] for s in sections), 'no room for another section header'
    assert image.data[header:header + 40] == b'\0' * 40
    rawptr = align(len(image.data), image.file_align)
    rawsize = align(len(blob), image.file_align)
    image.data += b'\0' * (rawptr - len(image.data)) + blob + b'\0' * (rawsize - len(blob))
    struct.pack_into('<8sIIIIIIHHI', image.data, header, SECTION_NAME, len(blob), va, rawsize, rawptr, 0, 0, 0, 0, 0x40000040)
    struct.pack_into('<H', image.data, image.pe + 6, image.count + 1)
    initialized = struct.unpack_from('<I', image.data, image.optional + 8)[0]
    struct.pack_into('<I', image.data, image.optional + 8, initialized + rawsize)
    struct.pack_into('<I', image.data, image.optional + 56, align(va + len(blob), image.section_align))
    field = image.optional + 64
    struct.pack_into('<I', image.data, field, checksum(image.data, field))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image.data)
    report.update(output=hashlib.sha256(image.data).hexdigest().upper(), outputBytes=len(image.data),
                  patches=dict(requirements=[f'0x{s:X}' for s in REQUIREMENT_SITES], admitted='0x190 (Ada)',
                               jumpEntry=f'0x{JUMP_ENTRY:X}'))
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'wrote {args.output} sha256={report["output"]}')


if __name__ == '__main__':
    main()
