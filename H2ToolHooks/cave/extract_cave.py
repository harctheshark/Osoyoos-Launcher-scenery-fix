import pefile, struct, sys
pe = pefile.PE('objfix_cave.dll')
base = pe.OPTIONAL_HEADER.ImageBase
size = pe.OPTIONAL_HEADER.SizeOfImage
hdrsz = pe.OPTIONAL_HEADER.SizeOfHeaders
raw = pe.__data__
# build the MAPPED image (headers + each section at its VirtualAddress)
img = bytearray(size)
img[0:hdrsz] = raw[0:hdrsz]
for s in pe.sections:
    d = s.get_data()
    rva = s.VirtualAddress
    img[rva:rva+len(d)] = d
# HIGHLOW (type 3) base relocations, as flat RVAs
rvas = []
if hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
    for blk in pe.DIRECTORY_ENTRY_BASERELOC:
        for e in blk.entries:
            if e.type == 3:
                rvas.append(e.rva)
rvas.sort()
# export RVAs for the stubs
exp = {}
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if e.name:
            exp[e.name.decode()] = e.address
open('objfix_cave.bin', 'wb').write(bytes(img))
with open('objfix_cave.reloc', 'wb') as f:
    f.write(struct.pack('<I', len(rvas)))
    for r in rvas:
        f.write(struct.pack('<I', r))
print('ImageBase =', hex(base), ' SizeOfImage =', hex(size), ' bin bytes =', len(img))
print('HIGHLOW relocs =', len(rvas))
for k in ('stub_scale', 'stub_gate', 'stub_decomp'):
    print('  %-14s RVA = %s' % (k, hex(exp.get(k, 0))))
# sanity: disasm the first bytes of each stub
try:
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for k in ('stub_scale', 'stub_gate', 'stub_decomp'):
        rva = exp.get(k, 0)
        if rva:
            print('--- %s @ RVA %s ---' % (k, hex(rva)))
            for i, ins in enumerate(md.disasm(bytes(img[rva:rva+40]), base + rva)):
                print('   %08X %s %s' % (ins.address, ins.mnemonic, ins.op_str))
                if i > 6: break
except Exception as ex:
    print('capstone skip:', ex)
