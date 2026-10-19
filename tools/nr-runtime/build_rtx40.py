"""Builds TRP's RTX 40 copy of NVIDIA's NR runtime 310.9.16. Run with python -I.

NVIDIA ships this runtime with kernels for RTX 50 (sm_120) only, as PTX plus
native code. This script compiles NVIDIA's own PTX for Ada (sm_89) with the
CUDA toolkit, lowering the few sm_90+ features in four modules (ada_lowering.py),
and adds those kernels next to NVIDIA's sm_120 code (byte for byte) and PTX
(the same text, without the carriage returns Windows extraction adds).

Host changes are limited to what loading them needs, and each patch site must
hold NVIDIA's original bytes first, so any other input fails instead of
producing a broken DLL:
- the D3D11/D3D12 feature requirements and CreateFeature's architecture switch
  admit Ada; Turing, Ampere and Hopper stay refused with NVIDIA's own error.
  CUDA and Vulkan requirements still say Blackwell; TRP uses D3D12;
- the 7 network and 8 helper kernel bundles point at new bundles in an
  appended read-only section, with exact lengths.
The result is unsigned: NVIDIA's certificate is removed because it no longer
matches. The qualified toolchain and output are pinned below; --unqualified
builds anything else, marked as such in the report. Usage:
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
from ada_lowering import require

OFFICIAL_SHA256 = '2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F'
QUALIFIED_TOOLCHAIN = 'Cuda compilation tools, release 13.4, V13.4.92 Build cuda_13.4.r13.4/compiler.38855100_0'
QUALIFIED_OUTPUT_SHA256 = '41145ACB065FBC99FEFD9374E87D5883BFDDBEA0ED64911CA5D22AAA484A58D2'
FATBIN_MAGIC = b'\x50\xed\x55\xba\x01\x00\x10\x00'
IMAGE_BASE = 0x180000000
SECTION_NAME = b'.trpnr'

# Kernel bundles: (name, how the runtime references it, length variable, original RVA).
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
        require(struct.unpack_from('<H', data, self.optional)[0] == 0x20B, 'expected PE32+')
        self.table = self.optional + size
        self.section_align, self.file_align = struct.unpack_from('<II', data, self.optional + 32)
        require(struct.unpack_from('<Q', data, self.optional + 24)[0] == IMAGE_BASE, 'unexpected image base')

    def sections(self):
        for i in range(self.count):
            o = self.table + 40 * i
            name, vsize, va, rawsize, rawptr = struct.unpack_from('<8sIIII', self.data, o)
            yield dict(name=name.rstrip(b'\0'), header=o, vsize=vsize, va=va, rawsize=rawsize, rawptr=rawptr)

    def offset(self, rva):
        for s in self.sections():
            if s['va'] <= rva < s['va'] + max(s['vsize'], s['rawsize']):
                return s['rawptr'] + rva - s['va']
        raise RuntimeError(f'RVA 0x{rva:X} is not in a section')

    def read(self, rva, size):
        o = self.offset(rva)
        return bytes(self.data[o:o + size])

    def write(self, rva, value, expected):
        o = self.offset(rva)
        require(self.data[o:o + len(value)] == expected, f'unexpected bytes at RVA 0x{rva:X}')
        self.data[o:o + len(value)] = value


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def sha256(data):
    return hashlib.sha256(data).hexdigest().upper()


def fatbin_at(image, rva):
    header = image.read(rva, 16)
    require(header[:8] == FATBIN_MAGIC, f'no fatbin at RVA 0x{rva:X}')
    return image.read(rva, 16 + struct.unpack_from('<Q', header, 8)[0])


def run(cuda, name, *args, cwd=None):
    command = [str(Path(cuda) / 'bin' / name), *map(str, args)]
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{name} failed ({result.returncode}): {" ".join(command)}\n{result.stdout}{result.stderr}')
    return result.stdout


def toolchain(cuda):
    versions = {name: ' '.join(run(cuda, name, '--version').strip().splitlines()[-2:]) for name in ('ptxas', 'cuobjdump', 'fatbinary')}
    require(len(set(versions.values())) == 1, f'mixed CUDA tools: {versions}')
    return versions['ptxas']


def unpack(cuda, folder, name, payload):
    """Native code and PTX of one fatbin, by SM; PTX text without carriage returns."""
    folder.mkdir()
    (folder / name).write_bytes(payload)
    run(cuda, 'cuobjdump', '-xelf', 'all', name, cwd=folder)
    run(cuda, 'cuobjdump', '-xptx', 'all', name, cwd=folder)
    cubins = {p.name.split('.')[-2]: p.read_bytes() for p in folder.glob('*.cubin')}
    ptx = {p.name.split('.')[-2]: p.read_bytes().replace(b'\r', b'') for p in folder.glob('*.ptx')}
    return cubins, ptx


def build_bundle(cuda, index, original, work):
    """NVIDIA's sm_120 cubin and PTX, plus an sm_89 cubin compiled from that PTX."""
    folder = work / f'bundle{index:02}'
    folder.mkdir()
    cubins, ptxs = unpack(cuda, folder / 'nvidia', 'nvidia.fatbin', original)
    require(set(cubins) == {'sm_120'} and set(ptxs) == {'sm_120'}, f'bundle {index}: expected sm_120 code and PTX only')
    nvidia_ptx = ptxs['sm_120']
    ptx = nvidia_ptx.decode('ascii')
    if index in LOWERED:
        ada, rewrites, _, _ = ada_lowering.lower(ptx, LOWERED[index])
    else:
        require(ptx.count('.target sm_120') == 1, f'bundle {index}: unexpected target')
        ada, rewrites = ptx.replace('.target sm_120', '.target sm_89'), {}
    inputs = {'ada.ptx': ada.encode('ascii'), 'nvidia.sm_120.cubin': cubins['sm_120'], 'nvidia.sm_120.ptx': nvidia_ptx}
    for name, data in inputs.items():
        (folder / name).write_bytes(data)
    run(cuda, 'ptxas', '-arch=sm_89', '-O3', 'ada.ptx', '-o', 'ada.sm_89.cubin', cwd=folder)
    ada_cubin = (folder / 'ada.sm_89.cubin').read_bytes()
    run(cuda, 'fatbinary', '--64', '--compress-all', '--create=trp.fatbin',
        '--image3=kind=elf,sm=89,file=ada.sm_89.cubin',
        '--image3=kind=elf,sm=120,file=nvidia.sm_120.cubin',
        '--image3=kind=ptx,sm=120,file=nvidia.sm_120.ptx', cwd=folder)
    payload = (folder / 'trp.fatbin').read_bytes()
    require(payload[:8] == FATBIN_MAGIC, f'bundle {index}: fatbinary output is not a fatbin')
    payload = payload[:16 + struct.unpack_from('<Q', payload, 8)[0]]
    # The new bundle must decode back to NVIDIA's sm_120 code and PTX and our sm_89 code.
    check_cubins, check_ptx = unpack(cuda, folder / 'check', 'trp.fatbin', payload)
    require(set(check_cubins) == {'sm_89', 'sm_120'} and set(check_ptx) == {'sm_120'}, f'bundle {index}: unexpected contents')
    require(check_cubins['sm_120'] == cubins['sm_120'], f'bundle {index}: sm_120 code changed')
    require(check_cubins['sm_89'] == ada_cubin, f'bundle {index}: sm_89 code changed')
    require(check_ptx['sm_120'] == nvidia_ptx, f'bundle {index}: sm_120 PTX changed')
    entries = re.findall(r'\.entry\s+(\w+)', ptx)
    require(entries and entries == re.findall(r'\.entry\s+(\w+)', ada), f'bundle {index}: kernel entries changed')
    return payload, dict(entries=entries, rewrites=rewrites, nvidiaSm120=sha256(cubins['sm_120']),
                         nvidiaPtx=sha256(nvidia_ptx), trpSm89=sha256(ada_cubin), bundle=sha256(payload), bundleBytes=len(payload))


def checksum(data, field):
    """The PE image checksum: a folded 16-bit sum skipping the checksum field, plus the length."""
    total = 0
    padded = bytes(data) + b'\0' * (len(data) % 2)
    for offset in range(0, len(padded), 2):
        if field <= offset < field + 4:
            continue
        total += padded[offset] | padded[offset + 1] << 8
        total = (total & 0xFFFF) + (total >> 16)
    return ((total & 0xFFFF) + (total >> 16)) + len(data)


def verify(data, report):
    """Reads the finished image back: sections, references and lengths."""
    image = Image(data)
    sections = list(image.sections())
    added = sections[-1]
    require(added['name'] == SECTION_NAME, 'appended section missing')
    require(struct.unpack_from('<II', image.data, image.optional + 112 + 8 * 4) == (0, 0), 'certificate directory present')
    require(added['rawptr'] + added['rawsize'] == len(image.data), 'data after the appended section')
    for (name, (kind, site), length, _), details in zip(BUNDLES, report['bundles']):
        rva = int(details['rva'], 16)
        if kind == 'lea':
            target = site + 7 + struct.unpack_from('<i', image.read(site + 3, 4))[0]
        else:
            target = struct.unpack_from('<Q', image.read(site, 8))[0] - IMAGE_BASE
        require(target == rva, f'{name}: reference does not reach its bundle')
        bundle = fatbin_at(image, rva)
        require(sha256(bundle) == details['bundle'], f'{name}: bundle bytes changed')
        require(struct.unpack_from('<Q', image.read(length, 8))[0] == len(bundle), f'{name}: length differs')
    field = image.optional + 64
    require(struct.unpack_from('<I', image.data, field)[0] == checksum(image.data, field), 'checksum differs')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--official', type=Path, required=True)
    parser.add_argument('--cuda', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--unqualified', action='store_true', help='allow another toolchain or output, marked unqualified')
    args = parser.parse_args()
    require(sys.flags.optimize == 0, 'run without -O: the safety checks must stay active')
    data = args.official.read_bytes()
    require(sha256(data) == OFFICIAL_SHA256, 'not NVIDIA NR 310.9.16')
    tools = toolchain(args.cuda)
    require(tools == QUALIFIED_TOOLCHAIN or args.unqualified, f'unqualified CUDA toolchain: {tools}')
    image = Image(data)
    sections = list(image.sections())
    last = sections[-1]

    # Drop NVIDIA's certificate, which follows the last section and no longer applies.
    security = image.optional + 112 + 8 * 4
    cert_offset, cert_size = struct.unpack_from('<II', image.data, security)
    require(cert_offset == last['rawptr'] + last['rawsize'] and cert_offset + cert_size == len(image.data),
            'certificate is not at the end of the file')
    del image.data[cert_offset:]
    struct.pack_into('<II', image.data, security, 0, 0)

    report = dict(official=OFFICIAL_SHA256, toolchain=tools, bundles=[])
    blob = bytearray()
    va = align(last['va'] + last['vsize'], image.section_align)
    with tempfile.TemporaryDirectory() as temporary:
        for index, (name, (kind, site), length, original_rva) in enumerate(BUNDLES):
            original = fatbin_at(image, original_rva)
            require(struct.unpack_from('<Q', image.read(length, 8))[0] == len(original), f'{name}: length differs')
            payload, details = build_bundle(args.cuda, index, original, Path(temporary))
            blob += b'\0' * (align(len(blob), 256) - len(blob))
            rva = va + len(blob)
            blob += payload
            if kind == 'lea':
                code = image.read(site, 7)
                require(code[0] in (0x48, 0x4C) and code[1] == 0x8D and code[2] & 0xC7 == 0x05, f'{name}: not lea')
                require(site + 7 + struct.unpack_from('<i', code, 3)[0] == original_rva, f'{name}: lea target differs')
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
    require(header + 40 <= min(s['rawptr'] for s in sections), 'no room for another section header')
    require(image.data[header:header + 40] == b'\0' * 40, 'section header slot is in use')
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

    verify(image.data, report)
    output = sha256(image.data)
    qualified = tools == QUALIFIED_TOOLCHAIN and output == QUALIFIED_OUTPUT_SHA256
    require(qualified or args.unqualified, f'output {output} differs from the qualified build')
    report.update(output=output, outputBytes=len(image.data), qualified=qualified,
                  patches=dict(requirements=[f'0x{s:X}' for s in REQUIREMENT_SITES], admitted='0x190 (Ada)',
                               jumpEntry=f'0x{JUMP_ENTRY:X}'))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image.data)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'wrote {args.output} sha256={output}{"" if qualified else " (UNQUALIFIED)"}')


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        sys.exit(f'build_rtx40: {error}')
