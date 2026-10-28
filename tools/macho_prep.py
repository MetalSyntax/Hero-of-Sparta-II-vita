#!/usr/bin/env python3
"""Prepara el ejecutable iOS (Mach-O 32 bits, MH_EXECUTE no PIE, armv6/armv7) para el loader de la PS Vita.

Soporta binarios thin (0xFEEDFACE) y fat/universal (0xCAFEBABE) seleccionando el slice ARM óptimo.
Detecta cifrado FairPlay (LC_ENCRYPTION_INFO).
Calcula con precisión exhaustiva los punteros absolutos que requieren reubicación en memoria física (rebase.bin):
  1. Literales en __TEXT.__text (ldr [pc, #imm]) que apuntan a __DATA, __cstring, o funciones en __text.
  2. Punteros en __DATA (__const vtables, __mod_init_func, __objc_*, __cfstring, __data).
  3. Stubs de imports dinámicos (__symbol_stub4).
  4. Símbolos locales en __nl_symbol_ptr.
Extrae el inventario completo de imports dinámicos y genera un reporte detallado.

Uso: python3 -I macho_prep.py <Executable> <rebase.bin> [--report report.txt] [--arch armv7|armv6]
"""
import os
import struct
import sys
from collections import Counter

LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_ENCRYPTION_INFO = 0x21

S_ZEROFILL = 0x1
S_NON_LAZY = 0x6
S_LAZY = 0x7

INDIRECT_LOCAL = 0x80000000
INDIRECT_ABS = 0x40000000

N_STAB = 0xE0
N_TYPE = 0x0E
N_SECT = 0x0E
N_ARM_THUMB_DEF = 0x0008

CPU_TYPE_ARM = 12
CPU_SUBTYPE_ARM_V6 = 6
CPU_SUBTYPE_ARM_V7 = 9
CPU_SUBTYPE_ARM_V7S = 11


def parse(path, preferred_arch=None):
    with open(path, 'rb') as f:
        raw_data = f.read()

    slice_offset = 0
    slice_size = len(raw_data)
    detected_arch = "unknown"

    # Chequear si es FAT Mach-O (big-endian 0xCAFEBABE)
    if len(raw_data) >= 8 and raw_data[:4] in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        magic, nfat = struct.unpack_from('>2I', raw_data, 0)
        archs = []
        for i in range(nfat):
            cputype, cpusubtype, offset, size, align = struct.unpack_from('>5I', raw_data, 8 + i * 20)
            arch_name = "arm"
            if cputype == CPU_TYPE_ARM:
                if cpusubtype == CPU_SUBTYPE_ARM_V6:
                    arch_name = "armv6"
                elif cpusubtype == CPU_SUBTYPE_ARM_V7:
                    arch_name = "armv7"
                elif cpusubtype == CPU_SUBTYPE_ARM_V7S:
                    arch_name = "armv7s"
            archs.append((arch_name, cputype, cpusubtype, offset, size))

        # Seleccionar slice
        chosen = None
        if preferred_arch:
            for a in archs:
                if a[0] == preferred_arch.lower():
                    chosen = a
                    break
        if not chosen:
            # Preferir armv7 sobre armv6
            for a in archs:
                if a[0] in ("armv7", "armv7s"):
                    chosen = a
                    break
        if not chosen:
            for a in archs:
                if a[0] == "armv6":
                    chosen = a
                    break
        if not chosen and archs:
            chosen = archs[0]

        if not chosen:
            raise RuntimeError("No se encontró ningún slice ARM en el binario FAT")

        slice_offset = chosen[3]
        slice_size = chosen[4]
        detected_arch = chosen[0]
        d = raw_data[slice_offset:slice_offset + slice_size]
    else:
        d = raw_data

    magic, cpu, sub, ftype, ncmds, _sz, flags = struct.unpack_from('<7I', d, 0)
    if magic != 0xFEEDFACE:
        raise RuntimeError(f"Se espera Mach-O de 32 bits (magic 0xFEEDFACE), obtenido 0x{magic:08X}")
    if ftype != 2:
        print(f"[!] Advertencia: Mach-O ftype={ftype} (se esperaba 2 = MH_EXECUTE)")

    if cpu == CPU_TYPE_ARM:
        if sub == CPU_SUBTYPE_ARM_V6:
            detected_arch = "armv6"
        elif sub == CPU_SUBTYPE_ARM_V7:
            detected_arch = "armv7"
        elif sub == CPU_SUBTYPE_ARM_V7S:
            detected_arch = "armv7s"
        else:
            detected_arch = f"arm(sub={sub})"

    segs, sects, symtab, dysym = [], [], None, None
    cryptid = 0
    off = 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', d, off)
        if cmd == LC_SEGMENT:
            name = d[off + 8:off + 24].rstrip(b'\0').decode('latin1', errors='ignore')
            vmaddr, vmsize, fileoff, filesize, _mp, _ip, nsects, _f = struct.unpack_from('<8I', d, off + 24)
            segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize))
            so = off + 56
            for _ in range(nsects):
                sn = d[so:so + 16].rstrip(b'\0').decode('latin1', errors='ignore')
                gn = d[so + 16:so + 32].rstrip(b'\0').decode('latin1', errors='ignore')
                addr, ssize, soff, _al, _ro, _nr, sflags, r1, r2 = struct.unpack_from('<9I', d, so + 32)
                sects.append(dict(seg=gn, name=sn, addr=addr, size=ssize, fileoff=soff,
                                  type=sflags & 0xFF, flags=sflags, r1=r1))
                so += 68
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from('<4I', d, off + 8)
        elif cmd == LC_DYSYMTAB:
            dysym = struct.unpack_from('<18I', d, off + 8)
        elif cmd == LC_ENCRYPTION_INFO:
            cryptoff, cryptsize, cryptid = struct.unpack_from('<3I', d, off + 8)
        off += size

    return d, segs, sects, symtab, dysym, detected_arch, cryptid


def main():
    if len(sys.argv) < 3:
        print("Uso: python3 -I macho_prep.py <Executable> <rebase.bin> [--report report.txt] [--arch armv7|armv6]")
        sys.exit(1)

    src, out = sys.argv[1], sys.argv[2]
    report = sys.argv[sys.argv.index('--report') + 1] if '--report' in sys.argv else None
    preferred_arch = sys.argv[sys.argv.index('--arch') + 1] if '--arch' in sys.argv else None

    d, segs, sects, symtab_cmd, dysym, detected_arch, cryptid = parse(src, preferred_arch)

    print(f"[*] Binario: {src}")
    print(f"[*] Arquitectura Mach-O: {detected_arch}")
    if cryptid != 0:
        print(f"[!] ALERTA CRITICA: El binario esta CIFRADO por FairPlay DRM (cryptid = {cryptid})!")
        print(f"[!] Debe ser descifrado en un dispositivo iOS (con Clutch, frida-ios-dump o bfdecrypt) antes de correr.")
    else:
        print(f"[+] Binario descifrado (cryptid = 0), listo para ejecución.")

    lo = min(s['vmaddr'] for s in segs if s['vmsize'] and s['name'] != '__PAGEZERO')
    hi = max(s['vmaddr'] + s['vmsize'] for s in segs if s['name'] != '__LINKEDIT')

    def read32(va):
        for s in segs:
            if s['vmaddr'] <= va < s['vmaddr'] + s['filesize']:
                return struct.unpack_from('<I', d, s['fileoff'] + va - s['vmaddr'])[0]
        return 0

    def sect_of(va):
        for s in sects:
            if s['addr'] <= va < s['addr'] + s['size']:
                return s
        return None

    # Parse symbol table
    names = []
    funcs = set()
    if symtab_cmd:
        symoff, nsyms, stroff, _strsize = symtab_cmd
        for i in range(nsyms):
            strx, ntype, nsect, ndesc, value = struct.unpack_from('<IBBhI', d, symoff + i * 12)
            idx_end = d.find(b'\0', stroff + strx)
            name = d[stroff + strx:idx_end].decode('latin1', errors='ignore') if idx_end != -1 else ""
            names.append(name)
            if (ntype & N_STAB) == 0 and (ntype & N_TYPE) == N_SECT:
                funcs.add(value & ~1)

    text_sec = next((s for s in sects if s['seg'] == '__TEXT' and s['name'] == '__text'), None)
    if not text_sec:
        raise RuntimeError("No se encontró sección __TEXT.__text")

    code_bytes = d[text_sec['fileoff']:text_sec['fileoff'] + text_sec['size']]

    # Function prologues and call targets in __text
    for i in range(0, len(code_bytes) - 3, 4):
        va = text_sec['addr'] + i
        w = struct.unpack_from('<I', code_bytes, i)[0]
        # push {..., lr} or str lr, [sp, #-4]!
        if (w & 0xFFFF4000) == 0xE92D4000 or w == 0xE52DE004:
            funcs.add(va)
        # bl target
        elif (w & 0x0F000000) == 0x0B000000:
            imm24 = w & 0x00FFFFFF
            if imm24 & 0x00800000:
                imm24 -= 0x01000000
            target = va + 8 + (imm24 << 2)
            if text_sec['addr'] <= target < text_sec['addr'] + text_sec['size']:
                funcs.add(target)
        # bx lr leaf function
        elif (w & 0x0FFFFFFF) == 0x012FFF1E:
            funcs.add(va)
            if i >= 4:
                prev = struct.unpack_from('<I', code_bytes, i - 4)[0]
                if i >= 8:
                    prev2 = struct.unpack_from('<I', code_bytes, i - 8)[0]
                    if (prev2 & 0xFFFF0000) in (0xE8BD0000, 0xE12F0000) or prev2 in (0xE1A00000, 0):
                        funcs.add(va - 4)

    # Initializer functions in __mod_init_func
    mod_sec = next((s for s in sects if s['name'] == '__mod_init_func'), None)
    if mod_sec:
        for a in range(mod_sec['addr'], mod_sec['addr'] + mod_sec['size'], 4):
            funcs.add(read32(a))

    # C++ vtables in __DATA.__const
    data_const_sec = next((s for s in sects if s['seg'] == '__DATA' and s['name'] == '__const'), None)
    if data_const_sec:
        for a in range(data_const_sec['addr'], data_const_sec['addr'] + data_const_sec['size'], 4):
            w = read32(a)
            if text_sec['addr'] <= w < text_sec['addr'] + text_sec['size'] and (w & 3) == 0:
                funcs.add(w)

    # Detect all PC-relative offset targets (PIC loads where register is later added to PC)
    # These contain relative offsets that must NEVER be rebased
    pc_rel_targets = set()
    for i in range(0, len(code_bytes) - 3, 4):
        va = text_sec['addr'] + i
        w = struct.unpack_from('<I', code_bytes, i)[0]
        if (w & 0x0E5F0000) == 0x041F0000:
            rd = (w >> 12) & 0xF
            u = (w >> 23) & 1
            imm12 = w & 0xFFF
            tva = (va + 8 + imm12) if u else (va + 8 - imm12)
            cur_reg = rd
            for j in range(4, min(256, len(code_bytes) - i), 4):
                nw = struct.unpack_from('<I', code_bytes, i + j)[0]
                # Track register copy: mov cur_reg, new_reg
                if (nw & 0x0FE0000F) == (0x01A00000 | cur_reg):
                    cur_reg = (nw >> 12) & 0xF
                # ADD reg, pc, cur_reg (or ADD reg, cur_reg, pc)
                if (nw & 0x0DE00000) in (0x00800000, 0x00400000): # ADD/SUB
                    rn = (nw >> 16) & 0xF
                    rm = nw & 0xF
                    if (rn == 15 and rm == cur_reg) or (rn == cur_reg and rm == 15):
                        pc_rel_targets.add(tva)
                        break
                # LDR reg, [pc, +/-cur_reg]
                if (nw & 0x0F200000) == 0x07000000:
                    rn = (nw >> 16) & 0xF
                    rm = nw & 0xF
                    if rn == 15 and rm == cur_reg:
                        pc_rel_targets.add(tva)
                        break
                # Function return or unconditional branch ends basic block
                if (nw & 0x0FFFFFFF) == 0x012FFF1E or (nw & 0x0FE00000) == 0x08A00000:
                    break

    rebase = set()
    stats = Counter()
    imports = Counter()
    handled_by_dynlink = set()

    # Track dynamic imports from LC_DYSYMTAB
    if dysym:
        indirectsymoff = dysym[12]
        extreloff, nextrel = dysym[14], dysym[15]
        for s in sects:
            if s['type'] in (S_NON_LAZY, S_LAZY):
                for i in range(s['size'] // 4):
                    idx = struct.unpack_from('<I', d, indirectsymoff + (s['r1'] + i) * 4)[0]
                    slot = s['addr'] + i * 4
                    if idx & INDIRECT_LOCAL:
                        rebase.add(slot)
                        stats[(s['name'], 'local')] += 1
                    elif not (idx & INDIRECT_ABS):
                        handled_by_dynlink.add(slot)
                        if idx < len(names):
                            imports[names[idx]] += 1
        for i in range(nextrel):
            addr, info = struct.unpack_from('<iI', d, extreloff + i * 8)
            handled_by_dynlink.add(addr)
            symnum = info & 0xFFFFFF
            if symnum < len(names):
                imports[names[symnum]] += 1

    # 1. Relocate literal pools in __TEXT.__text
    for i in range(0, len(code_bytes) - 3, 4):
        va = text_sec['addr'] + i
        w = struct.unpack_from('<I', code_bytes, i)[0]
        # ARM ldr rD, [pc, #+/-imm12]
        if (w & 0x0E5F0000) == 0x041F0000:
            u = (w >> 23) & 1
            imm12 = w & 0xFFF
            tva = (va + 8 + imm12) if u else (va + 8 - imm12)
            if not (text_sec['addr'] <= tva < text_sec['addr'] + text_sec['size'] - 3):
                continue
            if tva in pc_rel_targets:
                continue
            val = read32(tva)
            s = sect_of(val)
            if not s:
                continue
            if s['seg'] == '__DATA':
                rebase.add(tva)
                stats[('__text_lit', s['name'])] += 1
            elif s['seg'] == '__TEXT':
                if s['name'] in ('__cstring', '__ustring'):
                    rebase.add(tva)
                    stats[('__text_lit', s['name'])] += 1
                elif s['name'] == '__text':
                    if (val & 3) == 0 and val in funcs:
                        rebase.add(tva)
                        stats[('__text_lit', 'func')] += 1
                elif s['name'] in ('__const', '__gcc_except_tab'):
                    if (val & 3) == 0:
                        rebase.add(tva)
                        stats[('__text_lit', s['name'])] += 1

    # 1b. Relocate switch jump tables in __TEXT.__text
    # Pattern: ldr pc, [pc, rm, lsl #2] where the table starts at (va + 8)
    for i in range(0, len(code_bytes) - 3, 4):
        va = text_sec['addr'] + i
        w = struct.unpack_from('<I', code_bytes, i)[0]
        if (w & 0x0FFFFFF0) == 0x079FF100:
            table_start = va + 8
            cmp_limit = None
            for back in range(4, 28, 4):
                prev_pc = va - back
                prev_idx = prev_pc - text_sec['addr']
                if prev_idx < 0:
                    break
                prev = struct.unpack_from('<I', code_bytes, prev_idx)[0]
                if (prev & 0x0DF00000) == 0x01500000:
                    rot = (prev >> 8) & 0xF
                    val = prev & 0xFF
                    if rot > 0:
                        val = ((val >> (rot * 2)) | (val << (32 - rot * 2))) & 0xFFFFFFFF
                    cmp_limit = val + 1
                    break
                if (prev & 0x0DF00000) == 0x01A00000 and (prev & 0x000F0000) == 0:
                    rot = (prev >> 8) & 0xF
                    val = prev & 0xFF
                    if rot > 0:
                        val = ((val >> (rot * 2)) | (val << (32 - rot * 2))) & 0xFFFFFFFF
                    if cmp_limit is None and val < 256:
                        cmp_limit = val + 1

            limit = cmp_limit if (cmp_limit is not None and cmp_limit <= 512) else 512
            for idx in range(limit):
                ea = table_start + idx * 4
                e_off = ea - text_sec['addr']
                if e_off + 4 > len(code_bytes):
                    break
                dest = struct.unpack_from('<I', code_bytes, e_off)[0]
                if text_sec['addr'] <= dest < text_sec['addr'] + text_sec['size'] and (dest & 3) == 0:
                    rebase.add(ea)
                    stats[('__text_jumptable', 'entry')] += 1
                else:
                    break

    # 2. Relocate __symbol_stub4 targets
    stubs = next((s for s in sects if s['name'] == '__symbol_stub4'), None)
    if stubs:
        for a in range(stubs['addr'] + 8, stubs['addr'] + stubs['size'], 12):
            if read32(a - 8) == 0xE59FC000 and lo <= read32(a) < hi:
                rebase.add(a)
                stats[('__symbol_stub4', 'la_ptr')] += 1

    # 3. Relocate __DATA sections
    for s in sects:
        if s['seg'] != '__DATA':
            continue
        if s['type'] == S_ZEROFILL:
            continue
        if s['name'] == '__mod_init_func':
            for a in range(s['addr'], s['addr'] + s['size'], 4):
                v = read32(a)
                if lo <= v < hi:
                    rebase.add(a)
                    stats[(s['name'], 'init_fn')] += 1
        elif s['name'] in ('__objc_classlist', '__objc_protolist', '__objc_classrefs', '__objc_superrefs', '__objc_selrefs'):
            for a in range(s['addr'], s['addr'] + s['size'], 4):
                v = read32(a)
                if lo <= v < hi:
                    rebase.add(a)
                    stats[(s['name'], 'ptr')] += 1
        elif s['name'] in ('__objc_const', '__objc_data', '__const', '__data', '__cfstring'):
            for a in range(s['addr'], s['addr'] + s['size'], 4):
                v = read32(a)
                if not (lo <= v < hi):
                    continue
                ts = sect_of(v)
                if not ts:
                    continue
                if ts['name'] == '__text' and (v & 3) != 0:
                    continue
                rebase.add(a)
                stats[(s['name'], 'ptr')] += 1

    # Remove any slots handled at runtime by external dynamic relocations
    for slot in handled_by_dynlink:
        if slot in rebase:
            rebase.remove(slot)

    rebase_sorted = sorted(list(rebase))

    os.makedirs(os.path.dirname(os.path.abspath(out)) or '.', exist_ok=True)
    with open(out, 'wb') as f:
        f.write(struct.pack('<4sI', b'RBS1', len(rebase_sorted)))
        f.write(struct.pack(f'<{len(rebase_sorted)}I', *rebase_sorted))

    lines = [
        f'Binario: {src} ({detected_arch})',
        f'FairPlay cryptid: {cryptid} ({"CIFRADO" if cryptid else "descifrado"})',
        f'Rango imagen: {lo:#x}-{hi:#x}',
        f'Funciones identificadas en __text: {len(funcs)}',
        f'Rebase: {len(rebase_sorted)} palabras -> {out}',
    ]
    lines += [f'  {sn:22} {k:15}: {n}' for (sn, k), n in sorted(stats.items())]
    lines += [f'Imports dinámicos únicos: {len(imports)}']
    lines += [f'  {n}' for n in sorted(imports)]

    print('\n'.join(lines[:len(stats) + 5]))
    if report:
        os.makedirs(os.path.dirname(os.path.abspath(report)) or '.', exist_ok=True)
        with open(report, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
        print(f"[+] Reporte completo guardado en {report}")


if __name__ == '__main__':
    main()
