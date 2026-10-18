"""Builds TRP's RTX 40 kernel pack for NVIDIA's NR runtime 310.9.16. Run with python -I.

NVIDIA ships this runtime with kernels for RTX 50 (sm_120) only, as PTX plus
native code. This script compiles NVIDIA's own PTX for Ada (sm_89) with the
CUDA toolkit, lowering the few sm_90+ features in four modules (ada_lowering.py).
Each of the runtime's 15 kernel bundles is rebuilt with the sm_89 code next to
NVIDIA's sm_120 code (byte for byte) and PTX (the same text, without the
carriage returns Windows extraction adds).

The output is a kernel pack, not a DLL: the Universal renderer loads NVIDIA's
unmodified runtime and, on RTX 40 only, points its bundle references at these
bundles in memory (src/FrameGen/NeuralRuntimeAdaKernelPlan.cpp and
NeuralRuntimeAdaKernelsInternal.h hold the patch plan, checked here too).
Pack layout, little-endian:
    "TRPNRK1\\0", u32 version 1, u32 bundle count, 32-byte SHA-256 of the runtime,
    per bundle: u32 original RVA, u32 original bytes, u64 offset, u64 bytes,
    then the bundles, each 256-byte aligned.
The qualified toolchain and pack are pinned below; --unqualified builds
anything else, marked as such in the report. Usage:
    python -I build_rtx40_kernels.py --official nvngx_dlssnr.dll --cuda <CUDA root> --output <pack>
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
QUALIFIED_PACK_SHA256 = 'ACD2B6DD5526A71FFD5B34F78A9B7230DFDD57D5F79C581D10A876EB130AF542'
FATBIN_MAGIC = b'\x50\xed\x55\xba\x01\x00\x10\x00'
PACK_MAGIC = b'TRPNRK1\0'
PACK_VERSION = 1

# Kernel bundles: (name, how the runtime references it, length variable, original RVA).
# 'lea' sites are RIP-relative `lea reg, [bundle]`; 'pointer' sites are 64-bit
# absolute pointers in .data. The renderer patches these in memory.
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
# Admission sites the renderer also patches: the D3D11/D3D12 requirement
# minimum (`mov dword [rsp+0x3C], 0x1B0`) and CreateFeature's jump-table entry
# for architecture 0x190, which rejects until patched.
REQUIREMENT_SITES = (0x136E0, 0x15580)
REQUIREMENT_ORIGINAL = bytes.fromhex('c744243cb0010000')
JUMP_ENTRY, JUMP_REJECT = 0x18150, 0x17BDA
# These two descriptors cache their DWORD lengths during CRT initialization,
# before an in-memory installer can run. The installer replaces those copies
# too. The standalone builder changes the source scalars before CRT runs.
CACHED_LENGTHS = ((0x1100D0, 0x48C968), (0x110108, 0x48E488))
LENGTH_INITIALIZER = bytes.fromhex('8b05925548008905f48c10008b05a67048008905208d1000c3')


class Image:
    def __init__(self, data):
        self.data = bytes(data)
        self.pe = struct.unpack_from('<I', data, 0x3C)[0]
        optional = self.pe + 24
        count = struct.unpack_from('<H', data, self.pe + 6)[0]
        size = struct.unpack_from('<H', data, self.pe + 20)[0]
        require(struct.unpack_from('<H', data, optional)[0] == 0x20B, 'expected PE32+')
        self.base = struct.unpack_from('<Q', data, optional + 24)[0]
        self.sections = [struct.unpack_from('<8sIIII', data, optional + size + 40 * i) for i in range(count)]

    def read(self, rva, size):
        for _, vsize, va, rawsize, rawptr in self.sections:
            if va <= rva and rva + size <= va + min(vsize, rawsize):
                o = rawptr + rva - va
                return self.data[o:o + size]
        raise RuntimeError(f'RVA 0x{rva:X} is not in a section')


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


def check_sites(image):
    """The renderer's patch sites must hold NVIDIA's original bytes in this runtime."""
    for name, (kind, site), length, original_rva in BUNDLES:
        original = fatbin_at(image, original_rva)
        require(struct.unpack_from('<Q', image.read(length, 8))[0] == len(original), f'{name}: length differs')
        if kind == 'lea':
            code = image.read(site, 7)
            require(code[0] in (0x48, 0x4C) and code[1] == 0x8D and code[2] & 0xC7 == 0x05, f'{name}: not lea')
            require(site + 7 + struct.unpack_from('<i', code, 3)[0] == original_rva, f'{name}: lea target differs')
        else:
            require(struct.unpack_from('<Q', image.read(site, 8))[0] == image.base + original_rva, f'{name}: pointer differs')
    for site in REQUIREMENT_SITES:
        require(image.read(site, 8) == REQUIREMENT_ORIGINAL, f'requirement at 0x{site:X} differs')
    require(struct.unpack_from('<I', image.read(JUMP_ENTRY, 4))[0] == JUMP_REJECT, 'architecture jump entry differs')
    require(image.read(0x73D0, len(LENGTH_INITIALIZER)) == LENGTH_INITIALIZER, 'CRT length initializer differs')
    for cached, _ in CACHED_LENGTHS:
        require(image.read(cached, 4) == b'\0' * 4, f'cached length at 0x{cached:X} differs before CRT')


def write_pack(bundles):
    """bundles: (original RVA, original bytes, payload) in BUNDLES order."""
    header = PACK_MAGIC + struct.pack('<II', PACK_VERSION, len(bundles)) + bytes.fromhex(OFFICIAL_SHA256)
    offset = align(len(header) + 24 * len(bundles), 256)
    table, blob = bytearray(), bytearray()
    for rva, original_bytes, payload in bundles:
        blob += b'\0' * (align(offset + len(blob), 256) - offset - len(blob))
        table += struct.pack('<IIQQ', rva, original_bytes, offset + len(blob), len(payload))
        blob += payload
    pack = header + table + b'\0' * (offset - len(header) - len(table)) + blob
    return bytes(pack)


def read_pack(pack):
    require(pack[:8] == PACK_MAGIC, 'not a TRP NR kernel pack')
    version, count = struct.unpack_from('<II', pack, 8)
    require(version == PACK_VERSION and count == len(BUNDLES), 'unexpected pack version or bundle count')
    require(pack[16:48] == bytes.fromhex(OFFICIAL_SHA256), 'pack is for a different runtime')
    entries = []
    for i in range(count):
        rva, original_bytes, offset, size = struct.unpack_from('<IIQQ', pack, 48 + 24 * i)
        require(offset % 256 == 0 and offset + size <= len(pack), f'bundle {i}: out of bounds')
        payload = pack[offset:offset + size]
        require(payload[:8] == FATBIN_MAGIC and 16 + struct.unpack_from('<Q', payload, 8)[0] == size, f'bundle {i}: not a whole fatbin')
        entries.append((rva, original_bytes, payload))
    return entries


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
    check_sites(image)

    report = dict(official=OFFICIAL_SHA256, toolchain=tools, bundles=[])
    bundles = []
    with tempfile.TemporaryDirectory() as temporary:
        for index, (name, _, _, original_rva) in enumerate(BUNDLES):
            original = fatbin_at(image, original_rva)
            payload, details = build_bundle(args.cuda, index, original, Path(temporary))
            bundles.append((original_rva, len(original), payload))
            details.update(name=name, originalRva=f'0x{original_rva:X}', originalBytes=len(original))
            report['bundles'].append(details)
            print(f'{name}: {len(details["entries"])} kernels, {len(payload)} bytes')

    pack = write_pack(bundles)
    require([(r, n, sha256(p)) for r, n, p in read_pack(pack)] == [(r, n, sha256(p)) for r, n, p in bundles], 'pack read-back differs')
    output = sha256(pack)
    qualified = tools == QUALIFIED_TOOLCHAIN and output == QUALIFIED_PACK_SHA256
    require(qualified or args.unqualified, f'pack {output} differs from the qualified build')
    report.update(pack=output, packBytes=len(pack), qualified=qualified)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(pack)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'wrote {args.output} sha256={output}{"" if qualified else " (UNQUALIFIED)"}')


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        sys.exit(f'build_rtx40_kernels: {error}')
