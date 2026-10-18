# RTX 40 Neural Rendering kernels

`build_rtx40_kernels.py` builds a kernel pack for Universal's in-memory RTX 40
compatibility with NVIDIA NR 310.9.16. It requires the exact original runtime
and CUDA toolkit pinned in the script. Run without Python optimization:

```powershell
python -I tools/nr-runtime/build_rtx40_kernels.py --official <original-nvngx_dlssnr.dll> --cuda <CUDA-root> --output <folder>/nvngx_dlssnr_310.9.16_rtx40.trpk
```

Install the pack beside the original runtime. In the standard runtime folder,
name that original DLL `nvngx_dlssnr_310.9.16.dll`; retain `nvngx_dlssnr.dll`
(310.8) for fallback. Universal's Automatic selection tries the new runtime on
RTX 40; a missing, damaged or incompatible pack safely falls through to the
other installed choices. Explicitly selected original copies also use an
adjacent pack. Standard never installs kernels in memory. RTX 50 uses the
original runtime without the Ada patch. RTX 20/30 automatically try the bundled
community FP16 310.9.16 rebuild (`nvngx_dlssnr_310.9.16_compat.dll`) first, with
310.8 as fallback. That separate DLL is not produced by this RTX 40 builder.

Universal verifies both complete file hashes, loaded headers (accounting for
ASLR), original kernel bundles, all 35 patch sites, and signed displacement
reach. It only installs into a newly loaded module before NGX initialization.
An already resident module is accepted only when this installer owns its
recorded installation. NR initialization is serialized across passes.

The CPU-side pack allocation is read-only and stays resident, together with the
pinned module, for the process lifetime. Writes are verified and the instruction
cache is flushed. Protection restoration accepts Windows' conversion of touched
copy-on-write image pages to private writable pages, preserving the same access.
A failed transaction restores original bytes and protections before allowing
fallback. An unconfirmed rollback blocks runtime use and requires a restart.
No runtime file on disk is changed.

`build_rtx40.py` retains the separate standalone DLL build for comparison and
fallback. Its gameplay evidence does not establish acceptance of the in-memory
loader. Both builders preserve NVIDIA's RTX 50 cubins and PTX alongside the Ada
kernels. `--unqualified` explicitly labels development output; the renderer
accepts only the pinned qualified pack.

The `NeuralAdaKernels` CTest covers planning, malformed packs, displacement
limits, every partial write, page-protection failures, rollback, and competing
edits. Standard links a refusal stub. The test executable also accepts optional
absolute runtime and pack paths for a manual Windows loaded-image probe; this
does not initialize NGX. `tests/nr-peripheral` supplies the separate manual GPU
benchmark; configure `TRP_NR_VENDOR_BENCHMARK=ON` and `TRP_NR_ADA_KERNELS=ON`
to exercise the production Universal initialization path. Neither manual probe
runs as part of a build or CTest.
