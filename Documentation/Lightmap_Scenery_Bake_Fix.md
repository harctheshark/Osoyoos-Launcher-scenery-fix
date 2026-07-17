# Halo 2 Lightmapper — Scenery / Dynamic-Object Bake Fix

Covers both lightmapper targets:

- **MCC** — `tool_fast.exe` (stock MCC H2 tool, ImageBase `0x400000`, no ASLR)
- **Vista / 2007** — `H2Tool.exe` == `H2Tool_codez.exe` (same build; `codez` only adds a section)

Both fixes ship in `H2ToolHooks.dll`, which self-detects the tool at load (prologue check) and applies
the matching branch — a safe no-op on any binary that doesn't match. The launcher injects the DLL when
**Scenery Lightmap Fix** is enabled (env `OSOYOOS_INJECTOR_LIGHTMAP_SCENERY_FIX=1`).

Source: [`H2ToolHooks/LightmapSceneryFix.cpp`](../H2ToolHooks/LightmapSceneryFix.cpp).

---

## 1. The bug (shared root cause)

Halo 2 render_model geometry is stored with **compression-normalized vertex positions**: each section's
vertices are quantized to `[-1, 1]` against a per-section *compression bound* (the geometry's axis-aligned
min/max). At render time the engine decompresses them back to model space with

```
world = center + n * halfExtent          (per axis, n ∈ [-1, 1])
      = (min + max)/2  +  n * (max - min)/2
```

The **lightmapper never applies this decompression** on the scenery/dynamic-object path. It feeds the raw
`[-1, 1]` vertices straight into the occluder builder with only the object's placement transform (rotation +
translation, scale forced to `1.0`, **no compression bound**). The result is that every dynamic object is
treated as a raw ~`2×2×2` normalized cube:

- oversized / offset / blobby-black shadows on the ground,
- object per-vertex lighting color that doesn't match the surrounding structure lightmap.

This is a regression that predates 2007 — it affects **both** MCC and Vista (only the original 2004 Xbox tool
was correct). The fix is the same idea on both: **decompress the section's shared vertex buffer in place, once,
using the section compression bound**, gated to scenery only so BSP / instanced (poop) / render_model geometry
(which is already world-space) is left untouched.

Decode applied in place:

```
for each vertex v in the section's vertex buffer (196-byte stride, position at +0x00):
    v.x = halfExtent.x * v.x + center.x
    v.y = halfExtent.y * v.y + center.y
    v.z = halfExtent.z * v.z + center.z
```

Because the vertex buffer is shared by the occluder build **and** the per-vertex lighting gather, decompressing
the buffer once fixes both the shadow geometry and the lighting-color mismatch.

The two tools differ only in **where the addresses/structs live** and, critically, **where the compression
bound is stored** — see below.

---

## 2. MCC — `tool_fast.exe`

Two hooks (`apply_scenery_fix_mcc`), both on the stock MCC build:

| Hook | Function | Role |
|---|---|---|
| Scenery gate | `sub_49CF55` (occluder insert) | read the caller return address; mark scenery-only |
| Decompress | `sub_49DA6A` (add vertex) | decompress the section's vertex buffer in place, once per vbuf |

**Scenery gate.** `sub_49CF55` runs for *all* geometry classes. The handler reads the caller's return address
and sets a scenery flag only when the call came from the scenery import `sub_498962` `[0x498962, 0x49946B)`
(recursion within `sub_49CF55` inherits the flag; every other caller — BSP `sub_49843D`, instanced/poop
`sub_499DCE`, rmdl `sub_4AC600` — clears it). This is what keeps the decompression from shrinking BSP /
instanced geometry that is already correct.

**Compression bound (MCC layout).** The bound lives directly on the mesh's section struct:

```
mesh   = *a2                    (a2 = [esp+4] at sub_49DA6A)
vcount = *(mesh + 0x24)         (mesh+36)
vbuf   = *(mesh + 0x28)         (mesh+40)  — 196-byte verts, position @ +0x00, normalized [-1,1]
section= *(mesh + 0x04)         (mesh+4)
bound  =  section + 0x30        (section+48) — 6 floats (x0, x1, y0, y1, z0, z1)
```

`halfExtent = (x1 - x0)/2`, `center = (x1 + x0)/2`. Decompress once per vbuf (dedup on the vbuf pointer),
gated by the scenery flag and a normalized-vertex sanity check.

> Degenerate bound (e.g. `concrete_chunk_a`/`_h` on MCC, which carry an ~all-zero section bound): decompressing
> them collapses the verts to a point (zero-area tris the tracer ignores) — which correctly removes the
> phantom `2×2×2` occluder rather than leaving a fake shadow.

Delivery: originally shipped as a direct code cave in `tool_fast.exe`; now injected at runtime via
`H2ToolHooks.dll` (MD5-gated stock tool, farm-safe) so the tool binary is untouched.

---

## 3. Vista / 2007 — `H2Tool.exe` / `H2Tool_codez.exe`

A **single** naked JMP-detour (`apply_scenery_fix_h2v`) on the occluder-insert, scenery-gated by caller RA:

| Hook | Function | Role |
|---|---|---|
| Occluder insert | `sub_4A7420` | scenery-gate on caller RA, then decompress the section vbuf in place |

**Scenery gate.** `sub_4A7420` has exactly three callers. Only the scenery import `sub_49DB40`
`[0x49DB40, 0x49E744)` is scenery; the other two (`sub_49CB10`, `sub_49D690`) feed BSP / structure geometry
with an **identity** transform (already world-space, uncompressed) and are correctly excluded. The handler
gates on the return address being inside `sub_49DB40`.

**Section struct (Vista layout).** `a2` = the render section:

```
section + 0x00 : part count
section + 0x04 : parts pointer         — parts array, 72-byte stride
section + 0x24 : vertex count
section + 0x28 : vertex buffer         — 196-byte verts, position @ +0x00, normalized [-1,1]
```

**Compression bound (Vista) — the hard part.** Unlike MCC, the bound is **not** reliably in tool memory at all:

- The canonical one-per-section 56-byte `geometry_compression_info` block (`position_bounds` at `+0x00`,
  interleaved `(xmin,xmax,ymin,ymax,zmin,zmax)`) exists in the **.render_model tag file**, but the tool
  **strips it at load** — the runtime block reference at the section parent is `{count=0, data=-1}`.
- The 72-byte `geometry_part` struct carries a per-part model-space AABB at **`part+0x30`** whose union
  approximates the bound — but on models with the `Geometry Postprocessed` flag (e.g. the earthcity
  `concrete_chunk_*` debris) those boxes are **all zero**.
- Critically, vertices are normalized against the **MODEL-WIDE** bound, not the section's own extent
  (verified numerically on the crashed-pelican's 21 sections, and by DB generation: all 1745 models with
  records have identical per-section bounds). So the per-section parts-union is exact only for tight
  single-section models.

```
geometry_part (72 bytes):
  +0x00 type/flags   +0x04 material   +0x06 strip start   +0x08 strip index count   +0x0A/0x0C subparts
  +0x10 .. 0x1B      real_point_3d centroid          +0x20/+0x24/+0x28  node_weight[0..2]
  +0x30 .. 0x44      per-part model-space AABB (xmin,xmax,ymin,ymax,zmin,zmax) — zero on postprocessed models
```

**The section parent `P`** (104-byte render_model section element; stashed by two 6-byte mid-function detours
at `0x49DF50` / `0x49E163` where `P` sits in `eax` right before each occluder call) mirrors the tag fields as
14 uint16 words: `[2]`=vertex count, `[3]`=triangle count, `[4]`=part count, `[7]`=opaque points, …,
**`[13]` @`P+0x1A` = Geometry Compression Flags (bit0 = position compressed)**. `[P+0x38]` = the section
pointer (pairing check). These words are a strong per-section fingerprint.

### 3.1 Bounds database (`h2v_scenery_bounds.db`) — the definition library

Since the true bounds only exist in the tag files, an offline generator
([`Tools/generate_scenery_bounds_db.py`](../Tools/generate_scenery_bounds_db.py)) scans every
`*.render_model` under the tag root and emits one line per unique section fingerprint:
`14 words + 6 bound floats + ambiguity flag`. Tag files are parsed via their `dfbt` block markers
(sections = element size 0x68 blocks; compression records = element size 0x38; parts = 0x48 — records are
associated to sections by parts-block segmentation). Place the output **next to the `tags\` folder**
(the bake working directory); regenerate whenever render_model tags change:

```
py Tools\generate_scenery_bounds_db.py "F:\Digsite Leaked\halo2"

# MCC / H2EK tag set, restricted to scenery objects only:
py Tools\generate_scenery_bounds_db.py "G:\SteamLibrary\steamapps\common\H2EKMine" --scenery-only scenarios\objects objects
```

> `--scenery-only` keeps only render_models with a `.scenery` tag in the same folder — fewer entries and
> fewer ambiguous fingerprint collisions.
>
> **MCC branch is DB-wired too.** The MCC runtime value at `section+0x48` turned out to be `part[0]`'s AABB
> (not the compression bound; zero on postprocessed models — concrete chunks were silently collapsed to a
> point = no shadow, and multi-section models were subtly distorted, on every previous MCC bake). The MCC
> handler now resolves bounds from the DB by the MCC-reachable key **(vertex count @`mesh+0x24`, part count
> @`mesh+0x00`, total strip indices @`mesh+0x30`)** — the DB rows carry the strip total as their last column
> (rows are keyed on fingerprint + strip total; the Vista branch also prefers the strip-total-matching row,
> computing it as the sum of `part[i]+0x08`). `section+0x48` remains the fallback for DB misses, keeping the
> legacy collapse-on-zero. Verified on MCC earthcity_2: 38/40 scenery sections resolve via DB (concrete
> `±0.804/±1.012/±0.606` exact, pelican model-wide); the 2 misses are texcoord-only-compressed 3-vert markers
> whose fallback value is valid. The DB-loaded message prints to the tool console on both tools.

DLL search order: `OSOYOOS_H2V_BOUNDS_DB` env var → `tags\h2v_scenery_bounds.db` → `h2v_scenery_bounds.db`
(both relative to the tool's working directory). If missing, the fix logs a warning and falls back to the
parts-union (postprocessed models then stay undecompressed).

**Runtime resolution ladder** (per scenery section):

1. pairing check `[P+0x38] == section`; if flags bit0 = 0 → **skip** (verts already local);
2. **DB hit** (unambiguous) → use; cache `(P, bounds)`;
3. **sibling** — sections of the same model sit 104 bytes apart in one element array; since bounds are
   model-wide, any unambiguously-resolved sibling resolves the rest;
4. **ambiguous DB hit** (fingerprint shared by models with different bounds, e.g. identical-topology debris
   chunks; generator emits the largest-extent candidate) → use;
5. **parts-union** of `part[i]+0x30` (approximation) → use if non-degenerate;
6. otherwise leave the vertices untouched.

Then decompress the shared vbuf once: `world = halfExtent*v + center` from the chosen bound.

### 3.2 History: v1 → v2 → v4

- **v1** read 6 floats at `part[0]+0x28` — 8 bytes early (that's `node_weight[2]`, a constant 0, plus a
  shifted window). Effects: constant-0 x-min → offset+squish ("shadows too far"), true-x shifted into y and
  y into z → axis scramble ("rotated wrong"), under-scaled multi-part models ("dark/squished").
- **v2** read the correct `part+0x30` window and unioned all parts. Fixed single-section models (crates),
  but its "zero union = uncompressed, skip" assumption was wrong for postprocessed models: the concrete
  chunks are compressed with zero part boxes → left as raw 2×2×2 boxes → **bigger/darker shadows than v1**
  (which had collapsed them). Also subtly wrong on multi-section models (union < model-wide bound).
- **v4** (current) = flags word + tag-file DB + sibling/ambiguous/union ladder above.

### 3.3 Runtime verification (03a_oldmombasa / earthcity_2, v4)

Raw normalized verts → decompressed extents, checked against Guerilla's Compression Info values:

| Object | After v4 (model-local) | Source | Result |
|---|---|---|---|
| `concrete_chunk_a` (332v) | `±0.804, ±1.012, ±0.606` | DB (exact tag values) | regression fixed |
| crashed pelican (3480v of 21 sections) | `x[-4.78,5.05] y[-1.66,2.34] z[-1.41,1.46]` | DB, model-wide bound | correct sub-extent |
| `cov_guntower_base` (1575v of 4) | `x[-3.68,2.06] y[±3.22] z[-0.50,2.63]` | DB, model-wide bound | correct |
| chunk_h/i debris (38v) | `x[-0.30,0.23] y[±0.4] z[-0.04,0.07]` | ambiguous largest-extent | was a 2×2×2 box |
| tiny markers (3v, flags=2) | untouched | flags: not position-compressed | correct skip |

---

## 3.4 Placement SCALE respect (both tools)

The scenery importer builds the occluder transform with **scale hardcoded to 1.0**, so scaled placements
(scenario scenery datum scale, `0` reads as `1.0`) bake full-size shadows. Both tools' occluder vertex decode
(**Vista `sub_592C90`**, **MCC `sub_5AFB7F`**) computes `R·(m0·v) + T` with a real uniform-scale slot
`m0 = transform[0]` — confirmed in the MCC decompile, which explicitly multiplies `v` by `m0` when
`m0 != 1.0`. The hook runs *before* the tool copies the transform into the builder (Vista: the arg to
`sub_4A7420`; MCC: `qmemcpy(this, a4, 0x34)` in `sub_49CF55`), so the handler writes the placement's true
scale into `transform[0]` per placement. This is **per-placement** — the shared vertex buffer is never
touched, which is what makes it safe where the old MCC-era attempt (scaling rotation rows / vertices) wasn't
(same-model placements at different scales share one vertex buffer).

The scale is recovered by matching the transform's translation against the scenario scenery datums (block
`+128` count / `+132` ptr, stride 96, position `@+8`, scale `@+32`); scenario base = **Vista `sub_57C000()`**,
**MCC `*(0xC78CF4)` (`g_scenario`)**. Verified to match 100% of placements on both tools (0 misses); a failed
match changes nothing. Each scaled placement prints to the tool console:

```
MCC scenery scale: placement (x, y, z) scale=0.500 -> occluder scaled     (H2V on the Vista tool)
```

Env `OSOYOOS_H2V_SCALEMODE`: unset/`1` = apply true scale (default); `skip` = collapse scaled placements
(`m0=0` → zero-area tris → **no shadow at all**, the conservative fallback); `off` = disable. Only the
shadow/occluder geometry is scaled (not the per-vertex lighting sample positions).

## 4. Key addresses

### MCC `tool_fast.exe`
| Symbol | Address | Role |
|---|---|---|
| `sub_49CF55` | `0x49CF55` | occluder insert (scenery gate) |
| `sub_49DA6A` | `0x49DA6A` | add vertex (decompress) |
| `sub_498962` | `[0x498962, 0x49946B)` | scenery import (gate range) |
| bound | `section + 0x48` | 6 floats `(x0,x1,y0,y1,z0,z1)` |

### Vista `H2Tool.exe` / `H2Tool_codez.exe`
| Symbol | Address | Role |
|---|---|---|
| `sub_4A7420` | `0x4A7420` | occluder insert (hook point) |
| `sub_49DB40` | `[0x49DB40, 0x49E744)` | scenery import (gate range) |
| `sub_49CB10`, `sub_49D690` | — | BSP/structure callers (identity xform, excluded) |
| `sub_4A4FE0` / `sub_592C90` | — | add-vertex / position transform (no compression applied) |
| parent-stash hooks | `0x49DF50`, `0x49E163` | 6-byte detours capturing `P` (=`eax`) before each occluder call |
| parent `P` (104B) | `+0x00..0x1B` = 14 tag words; flags16 @`+0x1A` (bit0=pos-compressed); `[P+0x38]`=section | fingerprint + pairing |
| section | `+0x00` count, `+0x04` parts (72B), `+0x24` vcount, `+0x28` vbuf (196B) | |
| bound | `h2v_scenery_bounds.db` by fingerprint (model-wide) → sibling → ambiguous → union of `part[i]+0x30` | resolution ladder |

RE references (symbols build `H2ToolSymbols.exe.i64`): encoder `sub_85DA50`
(`geometry_definitions_new.cpp`), occluder box builder `sub_9D0A70` (`geometry_section_mopp.cpp`), section
import `sub_54AE80` (`render_geometry_import.cpp`).

---

## 5. Diagnostic

The Vista handler carries an env-gated dump (inert unless `OSOYOOS_H2V_LMDUMP=1`): for each scenery-gated
section it logs the placement transform, raw + decompressed vertex ranges, and every part's `+0x30` box, to a
scratchpad file. Useful for re-verifying a bound offset on a future tool build. It does nothing in production
(one cached env check).

Diagnostic bake (kill after geometry import — it runs early, before the long photon cast and before any tag
write):

```
inject_test.exe <H2ToolHooks.dll> "F:\Digsite Leaked\halo2" H2Tool_codez.exe ^
    lightmaps-slave "scenarios\solo\03a_oldmombasa\03a_oldmombasa" earthcity_2 medium 1 0
```

---

## 6. Build & delivery

```
# DLL
MSBuild H2ToolHooks\H2ToolHooks.vcxproj -p:Configuration=Release -p:Platform=Win32
#   -> PostBuild copies to Launcher\Resources\H2ToolHooks.dll (embedded resource)

# Launcher (embeds the DLL)
dotnet publish Launcher\ToolkitLauncher.csproj -c Release -r win-x64 -p:Platform=x64 ^
    -p:PublishSingleFile=true --self-contained false -p:SkipEditbin=true
#   -> Launcher\bin\x64\Release\net8.0-windows7.0\win-x64\publish\Osoyoos.exe
```

Enable **Scenery Lightmap Fix** in the launcher's build options and bake normally; the fix is injected at
runtime and the tool binaries are never modified.
