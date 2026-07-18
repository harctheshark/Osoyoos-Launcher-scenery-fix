# Confirmed fix: BSP Plücker edge-coefficient precision loss

## Summary

The bright, noisy indirect lighting in BSP geometry translated approximately
`-500` world units from the origin was caused by precision loss while
`tool_fast` constructed the BSP surface records used for ray intersection.

The faulty function is MCC `sub_49C09C`. It calculates six Plücker coordinates
for every mesh edge using single-precision products and subtraction. H2Tool's
paired implementation, `sub_4A01A0`, calls `sub_4C87B0`, which performs the same
calculation in double precision and converts only the final values to floats.

The fix replaces MCC `sub_49C09C` with an ABI-compatible implementation that
follows H2Tool's evaluation order. The corrected values are still stored in the
original FP32 surface-record layout, so no file format, tag layout, or downstream
ABI is changed.

The corrected hallway bake visually confirmed that this patch removes the
translation-dependent bright Monte Carlo noise.

**Delivery.** This fix ships in the Osoyoos Launcher as **"Lightmap Precision
Fix"** (Halo 2 MCC only). The six hooks originally prototyped as the standalone
`tool_fast_GPT.dll` (build id `GPT-FP64-SURFACE-EDGES-20260717-R7-4C87B0`, referred
to below as "R7") are folded into `H2ToolHooks.dll` and injected into every
`tool_fast` worker at launch. See [Osoyoos Launcher integration](#osoyoos-launcher-integration).

## Patch scope: root fix versus the complete build

The root-cause repair is the replacement of `sub_49C09C`. It is the change that
corrects the malformed triangle-edge data before ray tracing begins.

The shipped build is cumulative: in addition to replacing `sub_49C09C` it installs
five supporting precision hooks developed during the earlier R5 and R6
investigation:

| Address | Supporting change |
|---:|---|
| `0x4B4507` | Full FP64 static BSP surface test |
| `0x4B2DD4` | Full FP64 NUG/grid traversal and ray Plücker construction |
| `0x4B6D88` | FP64-intermediate sample position and normal interpolation |
| `0x4AE1DF` | FP64-intermediate `compute_light_value` origin bias |
| `0x4A8CEB` | FP64-intermediate final-texel origin bias |

The traversal (`0x4B2DD4`) and leaf (`0x4B4507`) hooks share a private
double-precision Plücker packet (magic `"GPF5"`) so the ray coordinates stay in
double across the traversal→leaf call instead of being re-read from FP32.

These five changes were visually ineffective without the producer repair and
are not additional root causes. They reduce downstream rounding once the correct
edge coefficients exist. Consequently, a minimal `sub_49C09C`-only build can fix
most of the defect while producing a lightmap that remains slightly different
from the complete six-hook build.

See [R7_EXTRA_PRECISION_HOOKS.md](R7_EXTRA_PRECISION_HOOKS.md) for the detailed
contribution, ranking, and isolation order of the five supporting hooks.

### The launcher now installs the fast path by default

The two most expensive supporting hooks — the full FP64 leaf test (`0x4B4507`)
and the full FP64 traversal (`0x4B2DD4`) — run per ray inside the final-gather
("computing ii entries") phase, which is ~80% of bake time. In side-by-side bakes
they produced **no visually observable change** in the lightmap once the producer
repair (`sub_49C09C`) was present; they only slowed the bake down. They are, in
practice, redundant.

The launcher therefore now always installs the **fast path**:

- `sub_49C09C` — the root edge-coefficient producer (the actual fix), and
- a cheap per-ray FP64 Plücker-moment recompute at `0x4B2F27` that stores the six
  corrected moment values back as FP32 and lets the stock (fast) traversal/leaf
  consume them.

The stock FP32 traversal and leaf run unchanged, so the bake runs at near-stock
speed while still consuming corrected edge data. The complete six-hook path is
still present in `LightmapPrecisionFix.cpp` (selected by the
`ApplyLightmapPrecisionFixFast` flag / `LIGHTMAP_PRECISION_FIX_FAST` variable) but
is no longer wired to any launcher control; the two modes are mutually exclusive.
To bring the full path back as an option, re-add a profile setting and gate
`usePrecisionFast` on it in `H2Toolkit.cs`.

## What was broken

Each BSP edge is defined by two FP32 positions, `p0` and `p1`. The ray tracer
represents that edge with six Plücker coefficients:

```text
p0.x * p1.y - p1.x * p0.y
p1.z * p0.x - p1.x * p0.z
p0.x - p1.x
p1.z * p0.y - p0.z * p1.y
p0.z - p1.z
p1.y - p0.y
```

MCC `sub_49C09C` evaluates the product differences as:

```text
mulss
mulss
subss
```

Both large products are therefore rounded to FP32 before they are subtracted.
When geometry is far from the world origin, the products contain large terms
caused by the translation even though the physical edge remains short. The final
coefficient is obtained by subtracting two nearly equal values, so this early
rounding is magnified by catastrophic cancellation.

For geometry near `-500`, one FP32 ULP is roughly `3.05e-5`. This is already
larger than the ray/surface acceptance tolerance of approximately `1e-5`.
Consequently, the rounded Plücker coefficient can move an edge-side result across
zero and classify a near-boundary ray on the wrong side of a triangle.

At the world origin, the large translation terms are absent and the same
calculation has substantially more useful precision. This is why moving the same
BSP to `0,0,0` hid the problem.

## Why the lighting became bright and noisy

Final gathering casts many randomized hemisphere rays from each lightmap sample.
Most rays were unaffected, but a small subset close to triangle boundaries were
misclassified by the damaged edge coefficients.

When an occluding hallway surface was missed, the gather ray continued into a
brighter surface, indirect-light source, or sky path. These failures did not
produce a uniform exposure change because only particular randomized rays landed
close enough to a damaged boundary. Instead, their excess radiance appeared as
sparse bright Monte Carlo noise.

With gathering disabled, this large population of randomized secondary rays and
its bright miss path were absent. That is why the defect was strongly associated
with gathering even though the actual precision loss occurred earlier, while the
BSP ray-intersection records were built.

## Why the earlier double-precision patches did not work

Several downstream functions were widened before the producer was identified:

- final gather;
- ray direction and origin calculations;
- origin normal bias;
- `sub_4B2DD4` grid traversal;
- `sub_4B4507` static BSP surface tests.

Those experiments were valid but visually unchanged because `sub_4B4507` still
read the six edge coefficients already stored in the 124-byte surface record at
offsets `+28..+96`. Converting an already-rounded float to a double preserves the
wrong float exactly; it cannot restore the bits discarded by `sub_49C09C`.

The important precision boundary was therefore upstream of both traversal and
final gather:

```text
FP32 vertex endpoints
        |
        v
sub_49C09C -- early FP32 product rounding -- precision permanently lost
        |
        v
124-byte BSP surface record
        |
        v
sub_4B2DD4 traversal -> sub_4B4507 edge tests -> final-gather result
```

Widening anything below the surface-record construction could make later
arithmetic more accurate, but it could not correct the boundary encoded in the
record.

## H2Tool comparison

The matching functions were established through their position in the paired
six-stage structure-import pipelines:

```text
tool_fast / MCC
import_structure_geometry  0x49843D
  sub_49B12C               surface-build pipeline
    sub_49C09C             sixth stage: edge coefficients

H2Tool
import_structure_geometry  0x49D690
  sub_4A54D0               paired surface-build pipeline
    sub_4A01A0             sixth stage: edge coefficients
      sub_4C87B0           Plücker numeric helper
```

H2Tool `sub_4C87B0` uses the following sequence for the product-difference
coefficients:

```text
cvtps2pd / cvtss2sd    promote endpoint floats
mulsd                   calculate both products in FP64
subsd                   subtract in FP64
cvtpd2ps / cvtsd2ss    round the final coefficient once to FP32
```

The final storage type is the same in both executables. The fix is not to change
the surface structure to contain doubles; it is to avoid intermediate FP32
rounding before the cancellation.

## Patch behavior

The fix installs a signature-verified entry jump at `0x0049C09C`. Its
replacement:

1. Iterates the original 60-byte edge records.
2. Reads the original 32-byte FP32 vertex records.
3. Promotes endpoint components to double.
4. Calculates all six coefficients using H2Tool's FP64 evaluation order.
5. Converts each completed coefficient once to float.
6. Copies the six floats into each adjacent 124-byte surface record.
7. Preserves the original edge orientation bits and record layout.

The build also retains the five supporting position, normal-bias, traversal, and
leaf hooks. Those changes were not sufficient by themselves and should not be
described as co-equal causes of the bug. Their role is to ensure the corrected
producer values are not subjected to avoidable downstream precision loss.

Of those supporting hooks, `0x4B4507` is expected to contribute the largest
remaining difference because it directly evaluates the repaired edge
coefficients. `0x4A8CEB` is expected to contribute the least because it affects
only the narrow final-texel origin-bias path.

## Osoyoos Launcher integration

The fix is exposed as the **"Lightmap Precision Fix"** checkbox on a Halo 2 MCC
profile. When enabled, the launcher sets the `LIGHTMAP_PRECISION_FIX` flag on the
single `H2ToolHooks.dll` injection so one injected DLL carries the precision fix,
the scenery fix, and any custom-quality patch together (no injection-slot
conflict). Implementation notes:

- **Source:** `H2ToolHooks/LightmapPrecisionFix.cpp` (the R7 six-hook
  implementation; `DllMain` replaced by `apply_lightmap_precision_fix(bool fast)`,
  invoked from `H2ToolHooks::hook()` when the flag is set). Built `/fp:strict`
  without LTCG to reproduce R7's codegen.
- **Fast path (default):** the launcher always pairs `LIGHTMAP_PRECISION_FIX` with
  `LIGHTMAP_PRECISION_FIX_FAST`, so `apply_lightmap_precision_fix(true)` installs
  only the root producer plus the cheap `0x4B2F27` moment recompute and skips the
  expensive per-ray FP64 leaf/traversal hooks. See *Patch scope* above for why the
  skipped hooks are redundant. The full six-hook path remains in the DLL, gated on
  the flag, for future use.
- **Signature gating:** every hook verifies its target's entry bytes before
  patching, so injection into any binary that is not stock MCC `tool_fast` (H2V
  `H2Tool`, byte-patched tools) is a safe no-op.
- **Injection timing:** the DLL must be injected *before* the lightmapper runs.
  The launcher's **"Keep Open"** output mode launches the tool through a `cmd`
  shell wrapper and can only inject after the tool is already running, which is
  too late. The lightmap path therefore forces **"Close Shell"** (a suspended
  launch that injects first) when a fix is active. **Close Shell** and **Silent**
  both work; Keep Open alone does not.

## Verification

Each injected worker emits, via `OutputDebugString` (viewable in DebugView), one
line confirming installation:

```text
[LM PRECISION R7] installed=true (producers=true trace/leaf=true edge=true)
   targets edge=0x49C09C interp=0x4B6D88 finalbias=0x4A8CEB clvbias=0x4AE1DF trace=0x4B2DD4 leaf=0x4B4507
```

A 16-worker MCC farm bake was verified at runtime: all six hook sites held an
`E9` entry jump in every worker, and the old mid-function cave address `0x49C0E6`
was untouched, confirming the whole-function replacement rather than the earlier
three-site approximation.

The standalone `tool_fast_GPT.dll` additionally writes a `.gptlog` file whose
`SURFACE_EDGE_BUILD_COMPLETE` record reports edges processed, coefficients copied,
and how many stored floats differ from MCC's original FP32 sequence — proving the
patch changed the upstream data rather than merely executing a mathematically
equivalent hook. That file logging is disabled in the Osoyoos build (it is
farm-unsafe with many concurrent workers); the `OutputDebugString` line above is
the equivalent per-worker confirmation.

## Reference binaries

The standalone prototype (not shipped in the launcher, kept for reference/A-B
testing):

```text
G:\SteamLibrary\steamapps\common\H2EKMine\tool_fast_GPT.exe
G:\SteamLibrary\steamapps\common\H2EKMine\tool_fast_GPT.dll
```

SHA-256:

```text
tool_fast_GPT.exe  7A149E9F7E9BD4CD400AA35DEC1F6FD842D15000E56796153E64B81C3DBA9A56
tool_fast_GPT.dll  EB014B735FDD0B171003428812AD85FE6391A9EB2818F9AF4847B2BF262B0AD8
```

The shipped artifact is the Osoyoos Launcher executable with the fix embedded in
`H2ToolHooks.dll`; it patches the unmodified stock `tool_fast.exe` in memory at
launch and leaves it unchanged on disk.

## Final diagnosis

The bug was not fundamentally Monte Carlo instability, sky-radiance evaluation,
or insufficient precision inside final gather. Monte Carlo gathering merely made
the underlying ray/triangle classification error visible.

The root cause was early FP32 rounding during translation-sensitive Plücker edge
construction. Matching H2Tool's double-precision intermediate calculation fixes
the triangle boundaries before any gather ray is traced, which is why the fix
works when the downstream-only patches did not.
