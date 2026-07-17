"""H2V lightmapper scenery-fix bounds DB generator.

Scans every *.render_model under <root>/tags and writes <root>/h2v_scenery_bounds.db:
one line per unique section fingerprint (14 uint16 words) -> position compression bounds
(6 floats: xmin xmax ymin ymax zmin zmax) + ambiguity flag.

Usage:  py generate_scenery_bounds_db.py <root> [--scenery-only] [tags-relative-subdir ...]

  <root>           folder that CONTAINS tags\\ (the bake working directory)
  --scenery-only   only include render_models with a .scenery tag in the same folder
  subdirs          optional tags\\-relative folders to restrict the scan (default: all of tags\\)

Examples:
  py generate_scenery_bounds_db.py "F:\\Digsite Leaked\\halo2"
  py generate_scenery_bounds_db.py "G:\\SteamLibrary\\steamapps\\common\\H2EKMine" --scenery-only scenarios\\objects objects

Re-run whenever render_model tags are added or changed. The injected H2ToolHooks scenery fix
loads the db from the bake working directory (the folder that contains tags\\), or from
tags\\h2v_scenery_bounds.db, or from the path in env var OSOYOOS_H2V_BOUNDS_DB.
"""
import struct, os, sys
if len(sys.argv) < 2:
    print(__doc__)
    sys.exit(2)
BASE = sys.argv[1]
SCENERY_ONLY = "--scenery-only" in sys.argv[2:]
SUBDIRS = [a for a in sys.argv[2:] if not a.startswith("--")]
TAGS = os.path.join(BASE, "tags")
ROOTS = [os.path.join(TAGS, s) for s in SUBDIRS] if SUBDIRS else [TAGS]
OUT = os.path.join(BASE, "h2v_scenery_bounds.db")
def valid_bounds(f):
    return (f[0]<f[1] and f[2]<f[3] and f[4]<f[5] and all(-1e5<x<1e5 for x in f[:6])
            and (f[1]-f[0])<4000 and (f[3]-f[2])<4000 and (f[5]-f[4])<4000)
def secwords_ok(w):
    cls,pad,vc,tris,parts,stris,sparts,opq,ovc,opc,nodes,srt,cls2,flags=w
    return (cls<=2 and pad==0 and 0<vc<50000 and 0<tris<50000 and 1<=parts<=128 and stris<=tris and sparts<=parts
            and opq<=vc and ovc<=vc and opc<=parts and (nodes&0xFF)<=8 and (nodes>>8)<=8 and srt<=tris and cls2<=2 and flags<=7)
allkeys={}   # key -> list of (bounds tuple or None)
stats=dict(files=0,sections=0,assoc=0,rootfb=0,nobounds=0,nosec=0,modelwide=0,not_modelwide=0,skipped_nonscenery=0)
def walk_all():
    for r in ROOTS:
        for dirpath,_,files in os.walk(r):
            yield dirpath, files
for dirpath,files in walk_all():
    has_scenery = any(f.lower().endswith(".scenery") for f in files) if SCENERY_ONLY else True
    for fn in files:
        if not fn.lower().endswith(".render_model"): continue
        if SCENERY_ONLY and not has_scenery:
            stats['skipped_nonscenery']+=1; continue
        p=os.path.join(dirpath,fn)
        try: d=open(p,'rb').read()
        except: continue
        stats['files']+=1
        blocks=[]; i=-1
        while True:
            i=d.find(b'dfbt',i+1)
            if i<0: break
            if i+16>len(d): break
            _,count,esz=struct.unpack_from('<3I',d,i+4)
            blocks.append((i,count,esz))
        sec=None
        for (pos,count,esz) in blocks:
            if esz==0x68 and count>=1 and pos+16+count*0x68<=len(d):
                ws=[struct.unpack_from('<14H',d,pos+16+k*0x68) for k in range(count)]
                if all(secwords_ok(w) for w in ws): sec=(pos,count,ws); break
        if not sec: stats['nosec']+=1; continue
        spos,scount,swords=sec
        root=None
        for (pos,count,esz) in blocks:
            if pos>=spos: break
            if esz==0x38 and count>=1:
                f=struct.unpack_from('<10f',d,pos+16)
                if valid_bounds(f): root=f[:6]
        persec=[None]*scount; striptot=[0]*scount; j=0; lastcomp=None; segok=True
        for (pos,count,esz) in blocks:
            if pos<=spos: continue
            if esz==0x38 and count>=1:
                f=struct.unpack_from('<10f',d,pos+16)
                if valid_bounds(f): lastcomp=f[:6]
            elif esz==0x48 and count>=1:
                if j<scount and count==swords[j][4]:
                    persec[j]=lastcomp; lastcomp=None
                    # total strip index count = sum of part element word[4] (strip length) - the MCC-side key
                    striptot[j]=sum(struct.unpack_from('<H',d,pos+16+k*0x48+8)[0] for k in range(count))
                    j+=1
                elif j<scount: segok=False
        if j!=scount: segok=False
        got=[b for b in persec if b is not None]
        if segok and got and all(max(abs(a-c) for a,c in zip(got[0],b))<1e-3 for b in got): stats['modelwide']+=1
        elif segok and got: stats['not_modelwide']+=1
        for k,w in enumerate(swords):
            stats['sections']+=1
            if not (w[13] & 1):
                stats['uncompressed']=stats.get('uncompressed',0)+1
                continue   # position NOT compressed: never emit (MCC side has no flags word to guard with)
            if segok and persec[k] is not None: b=persec[k]; stats['assoc']+=1
            elif root is not None: b=root; stats['rootfb']+=1
            else: b=None; stats['nobounds']+=1
            st=striptot[k] if segok else 0
            allkeys.setdefault((w,st),[]).append(None if b is None else tuple(b))
print("stats:",stats)
out=open(OUT,"w"); n=amb=0
for (key,st),bl in sorted(allkeys.items()):
    bs=[b for b in bl if b is not None]
    if not bs: continue
    uniq=[]
    for b in bs:
        if not any(max(abs(a-c) for a,c in zip(b,u))<1e-3 for u in uniq): uniq.append(b)
    if len(uniq)==1: b=uniq[0]; a=0
    else:
        b=max(uniq,key=lambda u:(u[1]-u[0])*(u[3]-u[2])*(u[5]-u[4])); a=1; amb+=1
    out.write(" ".join(str(x) for x in key)+" "+" ".join(f"{x:.6f}" for x in b)+f" {a} {st}\n"); n+=1
out.close()
print(f"db lines: {n} (ambiguous largest-extent: {amb})")
