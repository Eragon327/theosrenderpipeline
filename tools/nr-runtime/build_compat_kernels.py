"""Build Universal's NR 310.9.16 kernel pack without distributing another DLL.

Keep TRP's qualified SM89 code and NVIDIA's original SM120 code/PTX. Add the
SM75/SM86 cubins from the pinned ShortFuse/openNR compatibility runtime. No
model weights or host DLL are copied into the pack. The renderer applies the
separately audited host-admission and bundle-reference changes in memory.

Inputs are read-only. CUDA tools run offline; no DLL is loaded or GPU used.
Run with python -I, without -O. See README.md for input provenance.
"""
from pathlib import Path
import argparse
import json
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_rtx40_kernels as ada
from ada_lowering import require

COMPAT_SHA256 = 'CDFA6C1EC784867182164C7836F324F087B5C9F374D0C9FE46F54999FEABF3ED'
QUALIFIED_PACK_SHA256 = '751006ADB39C8DF0A7580EE095F8E605719AAA62832D6524DE30192E4A668FD3'
# The four NVAPI architecture jump-table entries, Turing through Ada. The
# original denies them; the community reference routes each to the same init.
ARCHITECTURE_SITES = ((0x18144, 0x17BCC), (0x18148, 0x17BD3), (0x1814C, 0x17BE1), (0x18150, 0x17BDA))


def referenced_bundle(image, index):
    _, (kind, site), _, _ = ada.BUNDLES[index]
    if kind == 'lea':
        instruction = image.read(site, 7)
        require(instruction[:3] == bytes.fromhex('4c8d05' if index < 7 else '488d05'), 'unexpected bundle reference')
        rva = site + 7 + struct.unpack_from('<i', instruction, 3)[0]
    else:
        rva = struct.unpack_from('<Q', image.read(site, 8))[0] - image.base
    return ada.fatbin_at(image, rva)


def build(cuda, original, compatibility, ada_pack, work):
    image, compat_image = ada.Image(original), ada.Image(compatibility)
    ada.check_sites(image)
    for site, old in ARCHITECTURE_SITES:
        require(struct.unpack_from('<I', image.read(site, 4))[0] == old, 'original architecture entry differs')
        require(struct.unpack_from('<I', compat_image.read(site, 4))[0] == 0x17C2B, 'community architecture entry differs')
    for site in ada.REQUIREMENT_SITES:
        require(compat_image.read(site, 8) == bytes.fromhex('c744243c60010000'), 'community requirement entry differs')
    ada_entries = ada.read_pack(ada_pack)
    bundles, details = [], []
    for index, (name, _, _, rva) in enumerate(ada.BUNDLES):
        folder = work / f'bundle{index:02}'
        folder.mkdir()
        original_payload = ada.fatbin_at(image, rva)
        original_cubins, original_ptx = ada.unpack(cuda, folder / 'original', 'original.fatbin', original_payload)
        compat_cubins, _ = ada.unpack(cuda, folder / 'community', 'community.fatbin', referenced_bundle(compat_image, index))
        ada_rva, ada_bytes, ada_payload = ada_entries[index]
        require((ada_rva, ada_bytes) == (rva, len(original_payload)), 'Ada bundle source differs')
        ada_cubins, ada_ptx = ada.unpack(cuda, folder / 'ada', 'ada.fatbin', ada_payload)
        require(set(original_cubins) == {'sm_120'} and set(original_ptx) == {'sm_120'}, 'original architecture set differs')
        require(set(compat_cubins) == {'sm_75', 'sm_86', 'sm_89', 'sm_120'}, 'community architecture set differs')
        require(set(ada_cubins) == {'sm_89', 'sm_120'} and ada_ptx == original_ptx, 'Ada architecture or PTX differs')
        require(compat_cubins['sm_120'] == ada_cubins['sm_120'] == original_cubins['sm_120'], 'original SM120 code was changed')
        cubins = {sm: compat_cubins[sm] for sm in ('sm_75', 'sm_86')}
        cubins.update(ada_cubins)
        images = []
        for sm, payload in cubins.items():
            filename = sm + '.cubin'
            (folder / filename).write_bytes(payload)
            images.append('--image3=kind=elf,sm=' + sm[3:] + ',file=' + filename)
        (folder / 'original.ptx').write_bytes(original_ptx['sm_120'])
        images.append('--image3=kind=ptx,sm=120,file=original.ptx')
        ada.run(cuda, 'fatbinary', '--64', '--compress-all', '--create=combined.fatbin', *images, cwd=folder)
        payload = (folder / 'combined.fatbin').read_bytes()
        require(payload[:8] == ada.FATBIN_MAGIC, 'combined output is not a fatbin')
        payload = payload[:16 + struct.unpack_from('<Q', payload, 8)[0]]
        checked_cubins, checked_ptx = ada.unpack(cuda, folder / 'check', 'check.fatbin', payload)
        require(checked_cubins == cubins and checked_ptx == original_ptx, 'combined code read-back differs')
        bundles.append((rva, len(original_payload), payload))
        details.append(dict(name=name, originalRva=f'0x{rva:X}', bytes=len(payload),
                            cubins={sm: ada.sha256(code) for sm, code in cubins.items()}, originalPtx=ada.sha256(original_ptx['sm_120'])))
        print(f'{name}: verified SM75/86 community, SM89 TRP and SM120 NVIDIA; {len(payload)} bytes', flush=True)
    pack = ada.write_pack(bundles)
    require(ada.read_pack(pack) == bundles, 'pack read-back differs')
    return pack, details


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    for name in ('official', 'compat', 'ada-pack', 'cuda', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--unqualified', action='store_true')
    args = parser.parse_args()
    require(sys.flags.optimize == 0, 'run without -O')
    original, compatibility, ada_pack = args.official.read_bytes(), args.compat.read_bytes(), args.ada_pack.read_bytes()
    require(ada.sha256(original) == ada.OFFICIAL_SHA256, 'original runtime identity differs')
    require(ada.sha256(compatibility) == COMPAT_SHA256, 'community runtime identity differs')
    require(ada.sha256(ada_pack) == ada.QUALIFIED_PACK_SHA256, 'qualified Ada input pack differs')
    tools = ada.toolchain(args.cuda)
    require(tools == ada.QUALIFIED_TOOLCHAIN, 'CUDA toolchain differs')
    with tempfile.TemporaryDirectory() as work:
        pack, details = build(args.cuda, original, compatibility, ada_pack, Path(work))
    digest = ada.sha256(pack)
    qualified = digest == QUALIFIED_PACK_SHA256
    require(qualified or args.unqualified, f'output {digest} is not the qualified pack')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(pack)
    report = dict(original=ada.OFFICIAL_SHA256, community=COMPAT_SHA256, adaPack=ada.QUALIFIED_PACK_SHA256,
                  toolchain=tools, pack=digest, packBytes=len(pack), qualified=qualified, bundles=details)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'wrote {args.output} sha256={digest} qualified={qualified}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        sys.exit(f'build_compat_kernels: {error}')
