# H2 Lightmapper — Scenery / Dynamic-Object Bake Fix (the "2×2×2 box" bug)

**Tool:** `tool_fast.exe` (H2 MCC offline lightmapper). Also broken in H2V/2007 (`H2ToolSymbols.exe`). RE done against `tool_fast.exe.i64` (no-ASLR, ImageBase 0x400000; same base funcs for the GPU-caved `tool_faster.exe`).

**Status:** ✅ **ROOT-CAUSED, FIXED, and SHIPPED (CPU)** — shadow geometry *and* per-vertex color both corrected. Reference implementation: `GpuLightmap/objfix.cpp` (→ `objfx.dll`). The remaining TODO is packaging (see [Delivery / open problems](#delivery--open-problems)).

This document is written so someone else can re-implement the fix from scratch, and so the traps we hit aren't re-hit.

---

## 1. The symptom

Dynamic objects (scenery, `device_machine`, etc.) baked in **per-vertex** lighting mode produce:

1. **Pitch-black, blobby, oversized shadows on the ground** under/behind the object. The shadow footprint is ~3× larger than the object and roughly cube-shaped, offset away from the object's real position.
2. **Object per-vertex lighting color doesn't match** the surrounding structure lightmap — objects get a warm/red or dark cast vs. the grey-blue ground.

Both are the **same root cause** (below). Open ground bakes fine; only object-affected texels/verts are wrong. The regression predates 2007 — only the original 2004 Xbox tool was correct (we don't have it), so there's **no working binary to diff against**. The lever we used instead is **object-path-vs-BSP-path within the same binary**.

---

## 2. Root cause — missing vertex decompression

Halo 2 `render_model` vertices are **compression-normalized**: stored as int16 normalized to `[-1, 1]` relative to a per-mesh-section **compression bounding box**. To get real-world-space vertices you must decompress:

```
world_local = vNorm * halfExtent + center        (per axis)
```

- **The renderer does this** when drawing the model → correct on-screen size.
- **The lightmapper's occluder-insert path SKIPS it.** It feeds the raw `[-1,1]` normalized verts straight into the occluder builder with `matrix = { m0=1.0, R=identity-rotation, T=placePos }`. Result: the occluder is a raw ~**2×2×2 normalized cube** sitting at the object's position, **not** decompressed to the true (e.g. 0.15 × 1.31 × 0.5) slab.

That oversized cube is what:
- **occludes sun/sky photons** over a ~3× area → a photon *hole* in the ground (see §3) → black shadow, and
- is **offset** by the bbox `center` (which is ≠ 0 for most meshes) → shadow sits away from the object, and
- looks **blobby/cube-ish** because it's literally a normalized cube.

The **color** bug is the *same missing decompression*: the per-vertex lighting gather (`sub_4A923A`) also transforms the normalized verts and then **samples the light field at those wrong (2×2×2-box) positions** → wrong colors.

> **Why it regressed:** older/uncompressed models stored raw true-scale verts, so "skip decompression" happened to be correct. Compressed models broke that assumption.

---

## 3. Dead ends & red herrings (do not re-chase these)

We burned real time on these. They are **not** the fix:

- **The `>8` photon gate** (`sub_4B897B @0x4B89AF`: `cmp edi,8 / jle → output 0`). Real, and it *does* floor photon-starved texels to exactly black — but that's a *consequence* of the photon hole, not the cause. Softening the gate (`GPULM_SOFTGATE`) only band-aids the black; the shadow is still oversized/offset. **Secondary/optional at best.**
- **"Objects don't bounce photons / aren't radiosity participants."** False. The photon tracer (`sub_497212`) already bounces off object geometry (no object-specific terminate branch). Objects already participate.
- **The placement transform / rotation matrix.** `sub_5AF523` builds a **pure rotation** (m0 hardcoded 1.0, T=placePos, no scale slot). Setting `matrix[0]=scale` has **no effect** (scenery is not the instanced `sub_4B4941` path where matrix[0] is the scale). Scaling the rotation rows **distorts** (breaks orthonormality → warped smear). The scale/size problem is in the **geometry (missing bbox), not the matrix rotation**.
- **Edge-propagation smear** (`sub_4A9EAE`) — suspected for "blobby," but it's downstream of the real bug.
- **`GPULM_OBJSCALE` row-scaling / `GPULM_SOFTGATE`** — empirical knobs we built while hunting; all produced "too small / too bright / too far." Band-aids, superseded by the real decompression fix.

---

## 4. The data structures (offsets)

All offsets are RVA-style / absolute in the no-ASLR `tool_fast.exe` image (base 0x400000).

**Mesh struct** `v7` (from the add-vertex hook, `v7 = *a2ptr`):
| Offset | Meaning |
|---|---|
| `v7 + 4` | pointer to **section** struct (heap) |
| `v7 + 36` | vertex count (int) |
| `v7 + 40` | **vbuf** — vertex buffer, **196 bytes/vert**, position (3 floats) at **+0**, normalized `[-1,1]` |

**Section struct** (`*(v7+4)`):
| Offset | Meaning |
|---|---|
| `section + 48` | **compression bbox** = 6 floats `(x0, x1, y0, y1, z0, z1)` |

Decompress params:
```
halfExtent h = ((x1-x0)/2, (y1-y0)/2, (z1-z0)/2)
center     c = ((x1+x0)/2, (y1+y0)/2, (z1+z0)/2)
world_local  = h * vNorm + c   (per axis)
```

**Worked example (a test post):** bbox `(-0.075, 0.075, -0.6428, 0.6837, -0.0019, 0.5)` → `h=(0.075, 0.6633, 0.2510)`, `c=(0.0, 0.0205, 0.2491)` → decompressed post = **0.15 × 1.31 × 0.5 slab** (matches the renderer). Raw `[-1,1]` occluder was ~13× / 1.5× / 4× too big per axis + z-offset = fat blobby square. ✔ confirms the model.

---

## 5. The occluder-transform chain (for reference / the matrix-bake alternative)

- Occluder point transform: `sub_5AFB7F(matrix a1, vert a2, out a3)` computes `out = R·(m0·vert) + T` where `m0=a1[0]`, `R=a1[1..9]`, `T=a1[10..12]`. Normal transform = `sub_5AFC38` (same, no T).
- Matrix built by `sub_5AF523` (rotation only, m0=1.0, T=0) inside `sub_498962`; `T` later filled with the object placement position (`v5+8/12/16`).
- Matrix layout in `a1`: `out[0]=a1[1]*vx+a1[4]*vy+a1[7]*vz+a1[10]` → col_x=`a1[1,2,3]`, col_y=`a1[4,5,6]`, col_z=`a1[7,8,9]`, T=`a1[10,11,12]`.

**Matrix-bake fix (the alternative we did *not* ship):** fold the bbox into the matrix so `sub_5AFB7F` produces `R·(h·vNorm + c) + pos`:
```
new_T[i] = T[i] + col_x[i]*cx + col_y[i]*cy + col_z[i]*cz   (using ORIGINAL columns)
then scale columns: col_x *= hx, col_y *= hy, col_z *= hz
```
We rejected this because the **color gather uses a *different* matrix** (@esi+0xC in `sub_4A923A`). Fixing only the occluder matrix would leave the color bug. **Decompressing the shared vertex buffer once fixes both consumers** — that's why the shipped fix modifies the vbuf, not the matrix.

---

## 6. The shipped fix (what to implement)

**Two hooks**, both inline `E9` detours with a trampoline that replays stolen bytes. Full working code in `GpuLightmap/objfix.cpp`. Summary:

### Hook A — scenery gate (`sub_49CF55`, the occluder-insert entry)
`sub_49CF55` runs for **all** geometry. We must only touch **scenery**. On entry, read the caller's return address and set a global flag:

- caller ∈ `[0x498962, 0x49946B)` → `sub_498962` = **scenery** → `g_sceneryActive = 1`
- caller ∈ `[0x49CF55, 0x49D326)` → recursion → **inherit** (leave flag)
- else (BSP `sub_49843D` @0x498515, poop `sub_499DCE` @0x499E7C, rmdl `sub_4AC600` @0x4AC722) → `g_sceneryActive = 0`

Stolen bytes at `0x49CF55` = 8 (`55 / 8D 6C 24 A0 / 83 EC 60` = push ebp / lea ebp,[esp-60h] / sub esp,60h), continue at `0x49CF5D`.

### Hook B — vbuf decompress (`sub_49DA6A`, add-vertex)
When `g_sceneryActive`, decompress the vertex buffer **in place, exactly once per vbuf**:

```c
if(!g_sceneryActive) return;                       // scenery only
v7  = *(unsigned*)a2ptr;  sec = *(unsigned*)(v7+4);
vb  = *(unsigned*)(v7+40); vcount = *(int*)(v7+36);
// gates (see §7) ...
bb  = (float*)(sec+48);
hx=(bb[1]-bb[0])*.5; hy=(bb[3]-bb[2])*.5; hz=(bb[5]-bb[4])*.5;
cx=(bb[1]+bb[0])*.5; cy=(bb[3]+bb[2])*.5; cz=(bb[5]+bb[4])*.5;
if(vbuf_seen_or_add(vb)) return;                   // once per vbuf
for(i=0;i<vcount;i++){ v=(float*)(vb+196*i); v[0]=hx*v[0]+cx; v[1]=hy*v[1]+cy; v[2]=hz*v[2]+cz; }
```

The vbuf is **shared** by the occluder builder *and* the per-vertex gather, so this one edit fixes shadow geometry **and** color together.

Stolen bytes at `0x49DA6A` = 8 (`83 EC 3C / A1 FC 7B BC 00` = sub esp,3Ch / mov eax,[00BC7BFC]), continue at `0x49DA72`.

> **Scale-respect nice-to-have:** multiply `h` (and `c`) by the placement scale `*(float*)(v5+32)` (0.0 → default 1.0) to make scenery respect the editor scale field, which stock tool ignores. Falls out for free once you're decompressing.

---

## 7. Gating — the part that will bite you

`sub_49DA6A` / `sub_49CF55` run for **BSP, instanced (poop), and rmdl** too, and **those verts are already correct** (world-space or already handled). If you decompress them you **shrink them to ~1×1×1 and corrupt the bake**. So the caller-based scenery gate (Hook A) is essential, and Hook B additionally self-gates defensively:

1. `g_sceneryActive` must be set (caller was `sub_498962`).
2. **First vertex must be normalized:** `|v0.x|,|v0.y|,|v0.z| ≤ 1.5` — excludes world-space BSP verts that slip through.
3. **Bbox sanity:** `0.001 < h{x,y,z} < 50`.
4. **Pointer sanity:** section/vbuf in `[0x400000, 0x40000000)`, `0 < vcount ≤ 200000`.
5. **Once-per-vbuf:** a small seen-set (`g_doneVbuf[1024]`) — the vbuf is shared, decompress it once or you'll double-apply.

Verified correct behavior: only the 3 scenery post vbufs (108/117/128 verts) got decompressed; poops and BSP untouched.

**Function ranges (for the gate):**
```
sub_498962 [0x498962, 0x49946B)   scenery import
sub_499DCE [0x499DCE, 0x49A247)   instanced/poop
sub_49843D [0x49843D, 0x498794)   BSP
sub_49CF55 [0x49CF55, 0x49D326)   occluder insert (recursion range)
sub_4AC600 [0x4AC600, 0x4AC93C)   rmdl
sub_4A923A [0x4A923A, 0x4A9C29)   per-vertex lighting gather (color consumer)
```
`sub_49CF55` callers: `0x498515`(BSP) · `0x498DDD`/`0x498FCE`(scenery) · `0x499E7C`(poop) · `0x4AC722`(rmdl).

---

## 8. Traps we hit (implementation gotchas)

- **`sub_49CF55` cannot be hooked with a call-based trampoline.** Its prologue is `lea ebp,[esp-60h]` — it sets `ebp` **absolutely from `esp`**. A call-based detour pushes an extra return frame → `esp` shifts 4 bytes → every `ebp`-relative local/arg is off → **crash within ~2 calls**, even a pure passthrough. **You must use a naked `JMP`-based detour** (no extra frame). This is why both hooks here are `__declspec(naked)` + `jmp [tramp]`, never `call`.
- **Silent build failures.** `GpuLightmap/build.bat` does **not** halt visibly on a `cl` error → a compile error (e.g. use-before-def) silently leaves the **stale DLL** loaded and you "test" old code. **Always** confirm the fresh `build ... TIME` line in the gpulm log advanced, and `grep error` the build output.
- **UNGATED bakes can permanently corrupt geometry.** If H2 `lightmaps` ever writes geometry back into the `structure_bsp` tag, an earlier ungated (BSP-decompressing) bake will corrupt instanced geo on disk. If that happened, **re-import** via structure-new-from-ass (`scenarios\multi\lighttest\structure\lighttest.ASS`). Gate *before* you run any real bake.

---

## 9. How we proved it (diagnostics, reusable)

- **Photon-hole render (root-cause proof).** `tool_faster.exe` + `gpulm.dll` with `GPULM_DUMPPHOT=1` dumps all photon world-positions → `reports\photons.bin`; `scratchpad\render_phot.py` (stdlib only) renders a top-down density map. Dark = holes. Clearly showed dark holes under scenery footprints (~5-unit footprint for ~1-unit posts) while the open field was covered → occluder is oversized. `render_crop.py` crops to a region for A/B (e.g. railing vs posts).
- **Occluder-vertex capture.** `GPULM_XFDUMP=1` (needs `GPULM_OBJSCALE=1`) → `[xf]` matrix dump + `[vtx]` vert capture via a wrap hook on `sub_5AFB7F`. This is how we caught the raw `[-1,1]` verts with `m0=1, R=I, T=placePos`.
- **Mesh/bbox dump.** Hook `sub_49DA6A` add-vertex → dump `v7+36` vcount, `v7+40` vbuf, `*(v7+4)+48` bbox. This located the compression bbox.
- **BSP-vs-object A/B.** Place an identical cluster of posts as BSP world geo (top) and as objects (bottom); object shadows were ~3× larger + blobby + wrong tint → isolated the object path.

Toggles (all in `gpulm.cpp`): `GPULM_OBJFIX=1` (matrix-bake variant), `GPULM_OBJFIX2=1` (vbuf-decompress w/ gate, for `tool_faster` testing), `GPULM_DUMPPHOT`, `GPULM_XFDUMP`, `GPULM_OBJSCALE`, `GPULM_SOFTGATE`.

---

## 10. Delivery / open problems

- **Shipped CPU deliverable:** `objfx.dll` (pure-CPU, hooks only the 2 funcs, no GPU — `GpuLightmap/objfix.cpp`, builds via `build_objfx.bat`) + `tool_fast_objfix.exe` (= `tool_faster.exe` with the loader string `"gpulm.dll"`→`"objfx.dll"` @ file 0x865A12; both names are 9 chars so it drops into the same `.gpl` loader slot).
- **Confirmed fixed:** shadow geometry (size/offset/blob) **and** per-vertex color, on both GPU (`tool_faster` `GPULM_OBJFIX=1`) and pure-CPU (`objfx.dll`). User-confirmed.
- **★ REMAINING TODO — package as a direct code cave in stock `tool_fast.exe`.** The user dislikes DLL sideloading: it breaks/annoys **Osoyoos**, which expects `tool_fast.exe` by name and runs its own tool patcher. Current stopgap: point Osoyoos `Settings.JSON → tool_fast_path` at `tool_fast_objfix.exe`. The clean end state is **two inline hooks patched directly into `tool_fast.exe`** — (A) the `sub_49CF55` scenery gate and (B) the `sub_49DA6A` vbuf decompress — with **no injection and no rename**. The logic is exactly `objfix.cpp`; it just needs to live in a `.text` code cave in the EXE itself (see `Fixes/Binary_Patch_Playbook.md` for the cave/rel32/verify recipe).

---

## 11. One-paragraph TL;DR for the next person

Scenery/dynamic-object shadows bake as oversized, offset, pitch-black cube blobs (and object colors are off) because the lightmapper inserts the `render_model`'s **compression-normalized `[-1,1]` vertices** as a shadow occluder **without applying the section compression bbox** (`section+48`, 6 floats) that the renderer uses to decompress them — so every object is a raw 2×2×2 box at its origin. Fix: hook `sub_49DA6A` (add-vertex) and decompress the vertex buffer in place (`world = h·vNorm + c`), **gated to scenery only** via the `sub_49CF55` caller check (must be `sub_498962`) — because that vbuf is shared by both the occluder builder and the per-vertex color gather, one decompress fixes shadows and color together. Use naked JMP detours (not call-trampolines — `sub_49CF55`'s `lea ebp,[esp-60h]` prologue crashes otherwise). Reference impl: `GpuLightmap/objfix.cpp`. Next step: bake it as a direct code cave into stock `tool_fast.exe` so Osoyoos works without sideloading.
