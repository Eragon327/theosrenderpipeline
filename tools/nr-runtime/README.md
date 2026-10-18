# Neural Rendering compatibility kernels

Standard retains the existing NR 310.8 DLL and original NVIDIA NR 310.9.16.
Universal supplies `nvngx_dlssnr_310.9.16.trpk`, a 12,215,592-byte kernel pack,
plus the retained community INT8 v2 DLL for RTX 20/30. The pack replaces the
separate full community and standalone RTX 40 310.9.16 DLLs in the release.
It contains GPU code and metadata, without duplicating the runtime's model
weights or host DLL. Explicit user-supplied runtime paths remain supported.

## Reproduce the combined pack

Run without Python optimization, using locally supplied inputs:

```powershell
python -I tools/nr-runtime/build_compat_kernels.py --official <original-310.9.16.dll> --compat <community-310.9.16.dll> --ada-pack <qualified-rtx40.trpk> --cuda <CUDA-root> --output <folder>/nvngx_dlssnr_310.9.16.trpk
```

The builder verifies every input's complete SHA-256 before parsing:

- Original NVIDIA 310.9.16:
  `2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F`.
- ShortFuse's community compatibility runtime:
  `CDFA6C1EC784867182164C7836F324F087B5C9F374D0C9FE46F54999FEABF3ED`.
  This is the reference reproduced by [openNR](https://github.com/clshortfuse/openNR)
  at `57ca170cb836f549f0e64544f282d535666b0f7d`; its source builder accepts the
  original NVIDIA file. The complete compatibility DLL is a build input only.
- Qualified TRP RTX 40 pack:
  `ACD2B6DD5526A71FFD5B34F78A9B7230DFDD57D5F79C581D10A876EB130AF542`.

Use the pinned CUDA 13.4 tools reporting V13.4.92. The builder extracts each
of 15 bundles, keeps the community SM75/SM86 cubins, TRP SM89 cubins and
NVIDIA SM120 cubins/PTX, then compresses them into one pack. Read-back must
reproduce every selected cubin and original PTX exactly. It also checks the
original and community host-admission sites. The qualified pack SHA-256 is
`751006ADB39C8DF0A7580EE095F8E605719AAA62832D6524DE30192E4A668FD3`.
An `--unqualified` output is explicitly marked and refused by the renderer.
No input DLL is loaded, edited or installed by either builder.

The retained `build_rtx40_kernels.py` produces the qualified RTX 40 input:

```powershell
python -I tools/nr-runtime/build_rtx40_kernels.py --official <original-310.9.16.dll> --cuda <CUDA-root> --output <folder>/nvngx_dlssnr_310.9.16_rtx40.trpk
```

It compiles NVIDIA's PTX for SM89 with narrow, audited instruction lowering.
`build_rtx40.py` retains the standalone DLL builder for manual comparisons;
that full DLL and the RTX 40-only input pack no longer ship in the release.
Source licensing and kernel/runtime attribution are in THIRD-PARTY.md.

## Runtime installation

Install the combined pack beside `nvngx_dlssnr_310.9.16.dll`, under
`SKSE/Plugins/TheosRenderPipeline/NVIDIA/`. Universal's Automatic selection
tries that original runtime on RTX 20/30/40. RTX 50 uses it without a patch.
RTX 20/30 use FP16 tensor-core fallback for the FP8 pipeline; RTX 40 keeps
native FP8. Missing, damaged or refused packs safely fall back to legacy
INT8 then 310.8 on RTX 20/30, or 310.8 on RTX 40. INT8 v2's exact known-bad
hash is refused on RTX 40. Standard links an installer refusal stub, never
modifies NR memory and keeps 310.8 on older GPUs.

Universal verifies both complete file hashes, loaded headers (accounting for
ASLR), original kernel bundles, all 38 patch sites and signed displacement
reach. This includes all four host architecture entries and two bundle
lengths cached by CRT initialization. It installs only into a freshly loaded
module before NGX initialization. A resident module is accepted only when this
installer owns and verifies its recorded installation. Initialization across
passes is serialized, and sibling assets use MO2's merged Data path.

The pack allocation becomes read-only and remains resident with the pinned
module for process lifetime. Writes are verified and the instruction cache
is flushed. Protection restoration accounts for private copy-on-write image
pages. Failure restores original bytes and protections before fallback;
unconfirmed rollback or changed installed storage blocks use until restart.
No runtime file on disk is modified.

`NeuralRuntimeKernels` CTest covers planning, malformed packs, displacement
limits, every partial write, protection failures, rollback, competing edits
and GPU admission. Supplying absolute runtime and pack paths to its manual
probe separately exercises real Windows mapping, pinning, concurrent reuse
and tamper detection; it does not initialize NGX. The optional benchmark in
`tests/nr-peripheral` exercises production feature creation and pixel readback
with `TRP_NR_VENDOR_BENCHMARK=ON` and `TRP_NR_ADA_KERNELS=ON` (the option keeps
its historical name). Manual probes are never launched by a build or CTest.

Byte-identical cubin preservation and static architecture checks do not
establish local RTX 20/30 hardware acceptance. Keep standalone pixel results,
actual game execution, user visual feedback and physical cadence separate.
