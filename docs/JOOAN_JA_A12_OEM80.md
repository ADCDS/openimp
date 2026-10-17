# JA-A12 OEM80 driver profile

This fork adds an explicit compatibility profile for the JOOAN W3-U / JA-A12,
T23N with CV2005/CV2005S1, retaining its OEM kernel and modules.

**Status:** source-only builds and boundary tests pass. The OEM80 reserved-rmem
IVDC workspace was allocated, adopted by the real driver, detached and freed
in two native probe cycles. The first cycle was kernel-clean; the second hit a
secondary AE kernel fault despite registering both sensors. Stable native
capture remains unqualified. The combined daemon also stops at an unported
secondary-FPS ioctl. See the [IVDC validation receipt](re/ja-a12-ivdc-validation-20261017.json).

## Build and selection

```sh
TOOLCHAIN_PREFIX=/path/to/toolchain/bin/mips-linux-gnu \
T23_EXTRA_CFLAGS='-muclibc -D_GNU_SOURCE=' \
T23_EXTRA_LDFLAGS=-muclibc ./build-t23.sh
```

The standalone compiler path does not require a Thingino build tree. The
existing Thingino path remains supported. Outputs are in `build/t23/`; no vendor
SDK library is required. The target uses uClibc 0.9.33.2. A bridge tempfile
compatibility path preserves atomic `O_EXCL | O_CLOEXEC` where `mkostemp` is absent.

Set these before starting a consumer compiled against OpenIMP's public API:

```sh
export OPENIMP_T23_DRIVER_ABI=oem80
export OPENIMP_T23_ENCODER=native
export OPENIMP_T23_NATIVE_FALLBACK=0
```

Unset/empty or `sdk88` preserves upstream behavior. Unknown names fail without
an ioctl downgrade/retry. ISP/native channels retain their selected profile;
changing the environment during use is unsupported. Proprietary worker launch,
including direct YUV and decoder entry points, refuses OEM80 and unknown modes.
The separate `jooanipc-open` project now recognizes this provider explicitly,
requires these three settings, and limits publication to sensor 0 with audio
disabled. Both physical sensors must still be registered: the retained
`direct_mode=2` AE worker visits both slots. This integration is not yet a
qualified capture replacement.

## VPU boundary recovered from the binary

The kernel dispatcher accepts `0xc0506300..0xc050630a`. A canonical 88-byte
OpenIMP channel record is translated into the OEM's 80-byte wire record.

| Field | OEM80 offset | SDK88 offset |
| --- | --- | --- |
| Channel / VPU cookies | 0x00 / 0x04 | same |
| Status / output length | 0x1c / 0x20 | same |
| Descriptor DMA address | 0x24 | same |
| Frame type / IVDC lines | 0x38 / 0x40 | same |
| Source-named `data_threshold`; semantics unresolved | 0x44 | same |
| 64-bit time | 0x48 | 0x50 |
| `max_bs_act` / reserved | absent | 0x48 / 0x4c |

The adapter copies the first 72 bytes and relocates time. Copy-back clears the
unavailable words, so time cannot become JPEG overflow status. Conservative
near-capacity JPEG checks remain enabled. H.264/JPEG REQUEST, RUN and RELEASE
and EPRC register reads all use the selected command family. Return values,
errno and possibly partial returned bytes are preserved; a nonzero return does
not establish valid encoded output.

Actual payload lengths differ from the encoded ioctl size: commands 0–4 copy
80 bytes; 5/7 use 12-byte cache/register records; 6/10 use 36-byte mapping
records; 8/9 use four-byte cookies. The new adapter exposes only channel
operations 0/1/2 and register read 7.

The existing full-NV12 input requirement, cache operations, retained VPU fd,
and `vlist=0` before CLOSE are preserved. OEM80 provisions the kernel IVDC scratch workspace. Native encoding of
IVDC line-ring input is not implemented; the encoder still requires complete
NV12 frames. The kernel flushes a fixed 1 MiB descriptor range; encoder
tests require exclusive reserved-memory ownership.

## ISP boundary recovered from the module

This ISP driver returns zero for unknown commands, so wrong ioctl words can
silently look successful. OEM80 uses the actual contracts:

| Operation | Command | Actual payload |
| --- | --- | --- |
| Register / unregister | `0x805456c1/c2` | 84 bytes |
| Enable / disable selection | `0xc0045627` | 8 bytes: `{enable, selector}` |
| Default IQ path | `0xc00456c7` | 128 zero-padded bytes; internal split unresolved |
| Get / set NCU buffer | `0x800c56d5/d4` | 12 bytes: `{selector, physical, size}` |
| Get / set WDR buffer | `0x800c56d7/d6` | same 12-byte form |

The selector is the low 16 bits at sensor-record **0x20**, named `reserved1` in
the public header. The tail `sensor_id` at 0x50 is a different field. Known
CV2005/CV2005S1 records use selectors 0/1, addresses 0x35/0x36, and I2C adapter
0 for both. Callers must supply valid complete 84-byte records.

The new per-slot helper tracks registration and NCU/WDR allocations. Setup
failure rolls back owned state. Failed unregistration retains potentially live
DMA buffers and refuses ISP close. Detached DMA-free failures can be retried.
Deleting one slot does not free the other's buffers.

FrameSource's 112-byte format, 20-byte REQBUFS and 68-byte V4L2 records match
the inspected module; driver-returned `sizeimage` stays authoritative. Sensor
enable/link construction, dual-sensor scheduling, IVDC allocation, interrupt
behavior and image tuning still need hardware qualification. Registration tests
alone do not establish those behaviors.

The first combined on-camera trial reached sensor enable, tuning, encoder
creation/binding and frame-channel format setup. REQBUFS then failed with
ENOMEM while IVDC requested its internal workspace from normal RAM. Although
about 14 MiB was free, no contiguous 1 MiB block remained. This is separate from
the capture frame pool in reserved memory. The OEM supplies a reserved-memory
IVDC workspace through `/dev/misc-ivdc`; the ownership-aware implementation
is described below. The single registered sensor also left slot 1 without AE
attributes, producing a separate kernel Oops. See the
[capture-interface ledger](re/ja-a12-capture-ledger.json) for the recovered
workspace commands, sizing and release sequence. A subsequent adapter change
registers both physical slots while publishing only sensor 0. The subsequent
repeat-cycle fault shows that registration alone does not complete secondary
AE activation or initialization. OEM80 now also
records successful global sensor/link startup so FrameSource does not repeat
those operations.
The [physical-trial receipt](re/ja-a12-camera-trial-20261017.json) separates the
failed capture run from the subsequent source changes and guarded retry.

## Reserved-memory IVDC workspace lifecycle

OEM80 FS0 caches the driver's direct mode during CreateChn. Before allocating
its frame pool or requesting buffers, it reads the line count and allocates a
separate `ivdcwork` extent from reserved rmem. The normal-RAM fallback is not
used for this resource. A 640-pixel channel with the retained 540-line setting
uses 622,080 bytes. The allocator reserves whole pages; the helper validates
rmem provenance, page alignment, size, and the complete physical extent below
the driver's 512 MiB KSEG0 limit. It checks WB+INV before publishing the address
through `SET_PADDR`.

After positive REQBUFS, the driver must report the same physical address in
`/proc/jz/isp/isp-ivdc` before frames are queued. This scratch allocation does
not replace capture buffers or relax the native encoder's full-NV12 checks.
Nonzero channels require FS0 to be enabled; dependent channels must stop before
FS0 releases the shared workspace.

Cleanup records queue/stream attempts before their ioctls. It issues STREAMOFF
only if STREAMON was attempted, joins the worker, releases the queue with
REQBUFS(0), verifies that the consumed DDR address is zero, and then clears any
unconsumed pending SET_PADDR. The frame FD closes before checked cache
maintenance and DMA release. The cached misc FD closes at DestroyChn.

Failure at any uncertain detach edge retains the descriptor, allocation ledger
and necessary FDs. DestroyChn retries partial EnableChn cleanup. Global system
exit and ISP teardown refuse live FrameSource resources instead of discarding
them. Fresh ISP and DMA initialization reject an already-consumed workspace.
Kernel Oops taint also blocks allocation, detach, free and global teardown;
resource-state observations in a damaged kernel are not accepted as reuse proof.
Lifecycle API calls must be serialized by the consumer. Unloading the library
or starting another process loses the local allocation ledger and is **not**
recovery from an uncertain detach; verify detachment or reset the device first.

`make -C tests/t23 -j2 ivdc-oem80` exercises checked sizes, cache errors,
partial publication, malformed ownership reports, stale allocation rejection,
primary/dependent ordering, and retained cleanup state through the actual
public DisableChn/DestroyChn/SystemExit paths. The seeded cleanup tests do not
exercise the full capture worker or every EnableChn error path. Native MIPS and
sanitizer runs supplement these host tests; physical results are recorded
separately. The native probe is built as `build/t23/openimp-t23-ivdc-probe`:

```sh
OPENIMP_T23_DRIVER_ABI=oem80 OPENIMP_T23_ENCODER=native \
OPENIMP_T23_NATIVE_FALLBACK=0 ./openimp-t23-ivdc-probe --run /private/path/libimp.so
```

Arrange exclusive media and watchdog ownership externally. The probe refuses
an existing media owner and uses existing sensor timings. It holds the process
and library mapping on failed teardown. Kernel health must remain clean; a
successful resource protocol does not establish fresh capture or repeat-safe
AE operation.

## Evidence and validation

- Kernel SHA-256: `1c2d0d2c4f07046af99e9c4f09c8b77ff62582c5f1d8fc7c6e7dc802cdac64ec`.
- ISP module SHA-256: `d82a86c36694f1da8e79b797a25309889433229f3bc42ffdbdfca3b382825907`.
  Module dispatch/copy offsets and capture contracts are in the
  [ISP evidence ledger](re/ja-a12-isp-ledger.json).
- Correct kernel base: **0x80010000**. The inherited Ghidra project was misbased
  at 0x80000000; only a scratch copy was corrected. Dispatcher: 0x801acafc;
  jump table: 0x803992e0. See the [binary ledger](re/ja-a12-vpu-ledger.json).
- REA 6.5's import gates reject these MIPS module/raw-image profiles. Analysis
  used direct Ghidra 12.1.3 and independent objdump without falsifying metadata.

Validate the recorded anchors against privately held firmware:

```sh
python3 tools/verify-ja-a12-abi.py --kernel /path/to/vmlinux.bin \
  --isp /path/to/tx-isp.ko
make -C tests/t23 -j2 check
make -C tests/t30 -j2 check
make -C tests/t30 -j2 oem80
```

Tests cover independent 80/88 layouts, nonzero timestamps, poisoned fields,
canaries, errors/cancellation, rollback, simultaneous sensors, DMA-free retry,
blocked worker launches and independently decoded fake-driver bitstreams.

`tools/t23_vpu_abi_probe.c` requests/releases one private channel, with no DMA,
RUN, ISP or sensor commands. On-camera SDK88 returned EPERM; OEM80 request and
release returned zero. `RTLD_NOW` library loading also passed. OEM services
stayed running and temporary files were removed. See the
[validation receipt](re/ja-a12-validation.json).

Build the limited probe separately with the matching compiler:

```sh
/path/to/toolchain/bin/mips-linux-gnu-gcc -muclibc -D_GNU_SOURCE= \
  -march=mips32r2 -Os -Isrc tools/t23_vpu_abi_probe.c \
  src/t30/t23_vpu_abi.c -o build/t23/t23-vpu-abi-probe
```

It runs only with `--request-release`; select the profile in the environment.

The upstream Helix self-test, including `-c`, allocates video buffers and
requires exclusive media ownership. No encoding/capture run is qualified here.
No OEM binary or decompiler export is added by this port. Licensing remains per file as
documented in upstream `NOTICE`; this fork does not relicense the existing tree.
