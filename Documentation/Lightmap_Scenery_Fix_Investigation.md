# How We Got Here — the Scenery Bake Fix Investigation

Companion to [Lightmap_Scenery_Bake_Fix.md](Lightmap_Scenery_Bake_Fix.md) (which documents *what* shipped).
This documents the **route**: every hypothesis, what evidence killed or confirmed it, and why the final
design is a tag-file database rather than anything simpler. Written so the next person (or the next session)
doesn't re-walk the dead ends.

Target: the Halo 2 lightmapper's scenery/dynamic-object path, on both **Vista/2007 H2Tool** and
**MCC tool_fast**. Test map throughout: `scenarios\solo\03a_oldmombasa`, BSP `earthcity_2`.

---

## 0. Starting point

The lightmapper feeds render_model vertices to its occluder builder with only the placement transform.
But those vertices are stored **compression-normalized** to `[-1,1]` — so every dynamic object baked as a
raw ~2×2×2 cube: oversized, offset, blobby-black shadows. The known fix (v1, ported from MCC to Vista) was:
decompress the vertex buffer in place, `world = center + n·halfExtent`, using 6 bound floats read from the
runtime structs. The entire investigation below is about **where those 6 floats actually live** — which
turned out to be a much harder question than it looked.

## 1. Three broken objects (the bug report)

With v1 on Vista, most scenery improved but three objects stayed wrong:

| Object | Symptom |
|---|---|
| `crate_tech_semi` | "rotated wrong, or using the 2×2×2 box" |
| `cov_guntower_base` | very dark, oversized shadow |
| `concrete_chunk_a` | shadows cast far away from the object |

Two competing hypotheses: **(A)** these objects reach the occluder through a call path the scenery gate
doesn't cover (never decompressed), or **(B)** multi-part sections were decompressed with the wrong part's
bound.

## 2. Static RE killed both initial hypotheses

Decompiling the occluder-insert's callers (IDA, `H2Tool.exe.i64` + the symbols build) proved:

- **(A) dead**: `sub_4A7420` has exactly 3 callers; the two non-scenery ones feed world-space BSP geometry
  with an identity transform. All scenery flows through the gated `sub_49DB40`. The broken objects *were*
  being decompressed — just wrongly.
- **(B) reframed**: one section = one shared vertex buffer = one bound. Granularity wasn't the issue.

Lesson that shaped everything after: *the fix was running; the **value** it used was wrong.*

## 3. Runtime dumps → the 8-byte window bug (v2)

We instrumented the hook (env-gated dump: per section, placement transform, raw vertex ranges, and the
72-byte part structs as raw floats) and baked earthcity_2. Two anomalies jumped out:

1. Every object's "x-min" was **exactly 0.0** — real bounding boxes don't do that. The field being read as
   x-min was a constant-zero field.
2. The floats 8 bytes later formed three clean `(min,max)` pairs with sane, symmetric values.

Verdict: v1 read `part[0]+0x28`, but that's `node_weight[2]` (a skinning weight, 0 for rigid scenery) —
**the true box is at `part+0x30`**. The mis-read shifted the whole window: garbage into x, true-x into y,
true-y into z, true-z dropped. That one bug explains all three symptoms: axis scramble ("rotated"),
x-offset+squish ("shadows too far"), under-scaled multi-part ("dark/oversized"). Two independent
decompilations (the vertex-compression encoder `sub_85DA50` and the mopp box builder `sub_9D0A70`) confirmed
the offset, the `(xmin,xmax,ymin,ymax,zmin,zmax)` ordering, and the decode formula.

**v2** = read `part+0x30`, union across parts, and *skip* sections whose boxes are all zero (assumed
"uncompressed"). Verified numerically in a bake — all three objects decompressed to sane extents.

## 4. The concrete regression — v2's two wrong assumptions

The user's rebake: crates fixed, but **concrete blocks got bigger, darker shadows than before**. v2 had
skipped them (zero boxes → "uncompressed"), leaving raw 2×2×2 cubes — worse than v1, which had at least
collapsed them flat.

Guerilla screenshots of the actual tags settled it:

- `concrete_chunk_a` **is** position-compressed (`Geometry Compression Flags` = position+texcoord), true
  bounds `x±0.804, y±1.012, z±0.606`. Its zero part-boxes are a side effect of the **`Geometry
  Postprocessed`** flag — the boxes are simply never populated for those models.
- Decoding the runtime section-parent struct `P` against the screenshots matched **word-for-word**
  (`332/206/3/141…`), locating the real compression-flags word at `P+0x1A` (an earlier guess at `P+0x26`
  had read garbage).
- The runtime `compression_info` block reference is `{count=0, data=-1}` **even though the tag file has the
  block** — the tool strips it at load. So for postprocessed models, *the true bounds do not exist anywhere
  in tool memory.*

And a second, independent discovery from cross-checking a 21-section mystery object (the **crashed
pelican**): every section of a model carries the **same model-wide** compression record — vertices are
normalized against the *model* bound, not the section's own extent (verified numerically: raw vertex range
= section extent ÷ model bound, exact). So v2's per-section parts-union was also subtly wrong for every
multi-section model. Generation-time stats later confirmed the model-wide property holds for **all 1745
record-bearing models, zero exceptions**.

## 5. Why a database (the "definition library")

At this point the constraints were fully known:

1. True bounds exist **only in the `.render_model` files** (stripped from memory on postprocessed models).
2. The hook has **no tag identity** (no name, no path) — only struct contents.
3. The tag files *can* be parsed offline: they carry explicit `dfbt` block markers (sections = 104-byte
   elements mirroring the runtime `P` words exactly; compression records = 56-byte blocks; parts = 72-byte
   blocks, which segment record→section association).

The user proposed exactly the right architecture: an offline **definition library**. A generator scans every
render_model, fingerprints each section by its counts words, and stores its compression bounds. At bake
time the hook fingerprints the runtime section the same way and looks the bounds up. Runtime ladder (Vista):

`compressed-flag check → DB hit → sibling reuse (same model ⇒ same bound, elements 104 bytes apart)
→ ambiguous-largest → parts-union → leave untouched`

Validation loop that kept this honest: 135 runtime sections from instrumented bakes + two oracle tags read
in Guerilla. First-pass association bugs (order-based record matching drifted on the 21-section pelican) were
caught **by that loop**, and fixed by parts-block segmentation. Verified end-to-end: concrete decompresses to
the exact Guerilla values; the greeble that is genuinely uncompressed (flags bit0 = 0) is left alone.

Ambiguity is real but bounded: topologically identical models with different scales (e.g.
`concrete_chunk_h` vs `_i`) share a fingerprint and **cannot** be told apart from runtime data — the
generator emits the largest-extent candidate, an error of centimeters on debris.

## 6. MCC: same disease, worse symptom, same cure

Wiring MCC (`tool_fast`) exposed that its trusted bound source — `section+0x48` — is actually **`part[0]`'s
AABB** too (vent_wall's runtime value matched part0's box, not the tag bound). Consequences of every prior
MCC bake: postprocessed models (concrete chunks) **collapsed to a point — silently missing shadows** (the
old "phantom object" rationalization was wrong), and multi-section models were subtly distorted.

MCC exposes no fingerprint words and no flags word, so it uses a reduced key that *is* reachable:
**(vertex count, part count, total strip indices)** — all three also derivable from the tag files. The DB
was extended to key rows on fingerprint **+ strip total** (which as a bonus disambiguated cross-model
fingerprint collisions like the pelican's LOD bits, and lets Vista pick the exact row too). `section+0x48`
remains MCC's fallback with legacy semantics. Verified: 38/40 earthcity_2 scenery sections resolve via DB;
the 2 misses are 3-vert texcoord-only markers whose fallback value is valid.

## 7. Dead ends worth remembering (so they aren't re-walked)

- **"Un-gated caller" theory** — plausible from symptoms, killed by a complete caller map. Get the caller
  map *first*; it's cheap.
- **`part[0]+0x28`** — looked runtime-verified originally, but the "verification" saw per-part differences
  and rationalized them. An always-exactly-0.0 field is a struct-misalignment alarm, not geometry.
- **Parts-union as the bound** — mathematically equal to the section bound *only* for tight single-section
  models; broke on model-wide normalization and on zero-box postprocessed models. It survives only as the
  last-resort fallback.
- **"Zero box = uncompressed, skip"** — wrong; zero boxes mean *unpopulated*, not uncompressed. Only the
  flags word (Vista) or DB absence-by-flag-filter (MCC) can say "uncompressed".
- **Canonical in-memory `compression_info`** — the architecturally "right" source, and it's simply not
  there at runtime (stripped, `{0,-1}`). Two RE passes confirmed the layout before runtime dumps proved the
  data was gone. When the canonical source is stripped, carry the data in from outside — hence the DB.
- **MCC's `section+0x48`** — trusted for months because it *looked* right on simple test objects. Same
  pattern as `+0x28`: a field that agrees with truth on single-part/section models and diverges exactly on
  the complex ones.

The meta-lesson repeated throughout: **fields that happen to equal the right value on simple objects are
not the right field.** Every wrong offset in this saga was "verified" on an object too simple to expose it.
The things that actually settled questions were: complete caller/structure maps, raw struct dumps compared
against Guerilla's ground truth, and an always-on validation set of real map sections.

## 8. Where everything lives

| Artifact | Location |
|---|---|
| Fix source (both tools) | `H2ToolHooks/LightmapSceneryFix.cpp` |
| DB generator | `Tools/generate_scenery_bounds_db.py` |
| Vista DB | `F:\Digsite Leaked\halo2\h2v_scenery_bounds.db` |
| MCC DB | `G:\SteamLibrary\steamapps\common\H2EKMine\h2v_scenery_bounds.db` |
| Launcher (embeds the DLL) | `Launcher\bin\x64\Release\net8.0-windows7.0\win-x64\publish\Osoyoos.exe` |
| Technical reference | [Lightmap_Scenery_Bake_Fix.md](Lightmap_Scenery_Bake_Fix.md) |
| Diagnostics | env `OSOYOOS_H2V_LMDUMP=1` (struct dumps + `MCCRES` resolution traces), env `OSOYOOS_H2V_BOUNDS_DB` (override DB path) |
