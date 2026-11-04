#!/usr/bin/env python3
"""Decompilation progress map for HOS2 -> docs/DECOMP_PROGRESS.md.

Uso: python3 -I tools/decomp_progress.py [--repo .]

Fuentes (todas locales, nada del juego sale al MD: solo direcciones, conteos y nombres de símbolos):
  game_app/HOS2                                Mach-O (rango de __TEXT,__text)
  decompiled/disasm/full_HOS2.md               objdump completo + índice de símbolos
  decompiled/HOS2_arm/ghidra/out_ghidra.c      pseudo-C de Ghidra
  decompiled/macho/objc_classes.txt            métodos ObjC (imp)
  decompiled/macho/imports.txt                 imports dinámicos
  source/*.c, *.md del repo                    direcciones ya analizadas / funciones reemplazadas
"""
import argparse, collections, datetime, glob, os, re, struct

ap = argparse.ArgumentParser()
ap.add_argument("--repo", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
R = os.path.abspath(ap.parse_args().repo)
P = lambda *a: os.path.join(R, *a)

# ---------------------------------------------------------------- Mach-O: __text
d = open(P("game_app", "HOS2"), "rb").read()
off = 0
if struct.unpack_from("<I", d, 0)[0] == 0xBEBAFECA:  # fat
    for i in range(struct.unpack_from(">I", d, 4)[0]):
        ct, _, o, _, _ = struct.unpack_from(">5I", d, 8 + i * 20)
        if ct == 12:
            off = o
ncmds = struct.unpack_from("<I", d, off + 16)[0]
p, text = off + 28, None
for _ in range(ncmds):
    cmd, sz = struct.unpack_from("<2I", d, p)
    if cmd == 1:
        nsect = struct.unpack_from("<I", d, p + 48)[0]
        for s in range(nsect):
            q = p + 56 + s * 68
            sect, seg = d[q:q + 16].rstrip(b"\0"), d[q + 16:q + 32].rstrip(b"\0")
            if (seg, sect) == (b"__TEXT", b"__text"):
                text = struct.unpack_from("<2I", d, q + 32)
    p += sz
T0, TSZ = text
T1 = T0 + TSZ
in_text = lambda a: T0 <= a < T1

# ---------------------------------------------------------------- disassembly
sym = {}  # addr -> name (symbol index)
insn = 0
with open(P("decompiled", "disasm", "full_HOS2.md"), errors="replace") as f:
    for line in f:
        m = re.match(r"0x([0-9a-f]{8})  (\S+)", line)
        if m:
            sym[int(m.group(1), 16)] = m.group(2)
        elif re.match(r"[0-9a-f]{8}\t[0-9a-f]{4,8}\t", line) and in_text(int(line[:8], 16)):
            insn += 1

# ---------------------------------------------------------------- ObjC methods
objc = {}  # imp -> Class::sel
cls = None
with open(P("decompiled", "macho", "objc_classes.txt"), errors="replace") as f:
    sel = None
    for line in f:
        m = re.match(r"\s+name\s+0x[0-9a-f]+ (\S+)", line)
        if m:
            if line.startswith("        name ") or line.startswith("        name\t"):
                cls = m.group(1)
            sel = m.group(1)
        m = re.match(r"\s+imp\s+0x([0-9a-f]+)", line)
        if m and sel:
            objc[int(m.group(1), 16)] = f"{cls}::{sel}"

# ---------------------------------------------------------------- Ghidra
fun_q = {}  # addr -> "ok" | "warn" | "bad"
fun_tag = {}  # addr -> library, from the assert() source paths the body references
# Only .cc/.cpp/.c paths: header asserts (uvector.h, tinyxml.h) are inlined into game code.
TAGS = (("ustl/memblock.cc", "uSTL"), ("tinyxml.cpp", "TinyXML"), ("tinyxmlparser.cpp", "TinyXML"), ("mpc_", "vox (audio engine)"), ("stb_vorbis", "vox (audio engine)"),
        ("GLLive", "GLLive (online)"), ("gllive", "GLLive (online)"))
named_ghidra = 0
total_ghidra = 0
cur, body = None, []
def close():
    if cur is None:
        return
    b = "".join(body)
    # Only warnings that make the pseudo-C wrong count ("does not return", "unknown calling convention" are cosmetic).
    q = ("bad" if ("halt_baddata" in b or "bad instruction" in b) else
         "warn" if ("Could not recover jumptable" in b or "Type propagation" in b) else "ok")
    if cur >= 0:
        fun_q[cur] = q
        for key, lib in TAGS:
            if key in b:
                fun_tag[cur] = lib
                break
with open(P("decompiled", "HOS2_arm", "ghidra", "out_ghidra.c"), errors="replace") as f:
    prev = ""
    for line in f:
        if line.startswith("{") and re.match(r"[A-Za-z_].*\)\s*$", prev):
            close()
            total_ghidra += 1
            m = re.search(r"\bFUN_([0-9a-f]{8})\(", prev)
            cur = int(m.group(1), 16) if m else -1
            named_ghidra += 0 if m else 1
            body = []
        elif cur is not None:
            body.append(line)
        if line.strip():
            prev = line
    close()

# ---------------------------------------------------------------- functions = union of known starts
starts = sorted({a for a in list(fun_q) + list(sym) + list(objc) if in_text(a)})
funcs = []
for i, a in enumerate(starts):
    e = starts[i + 1] if i + 1 < len(starts) else T1
    funcs.append([a, e - a])

def lib_of(name):
    n = name.lstrip("_")
    for key, lib in (("3vox", "vox (audio engine)"), ("vox", "vox (audio engine)"), ("gllive", "GLLive (online)"),
                     ("CGLLive", "GLLive (online)"), ("ustl", "uSTL"), ("St", "libstdc++"), ("GLT", "GLT math"),
                     ("glf", "glf"), ("ZN", "C++ (juego/otros)")):
        if key in n[:12]:
            return lib
    return "C/otros"

# ---------------------------------------------------------------- port knowledge
doc_hits = set()
files = glob.glob(P("source", "*.c")) + glob.glob(P("source", "*.h")) + glob.glob(P("*.md")) + glob.glob(P("docs", "*.md"))
for fn in files:
    if fn.endswith("DECOMP_PROGRESS.md"):
        continue
    for m in re.finditer(r"0x([0-9A-Fa-f]{5,6})\b", open(fn, errors="replace").read()):
        a = int(m.group(1), 16)
        if in_text(a):
            doc_hits.add(a)
src = "".join(open(f, errors="replace").read() for f in glob.glob(P("source", "*.c")))
hooked = set(int(m.group(1), 16) for m in re.finditer(r"macho_hook\(\s*(?:MACHO_SLIDE\s*\+\s*)?0x([0-9A-Fa-f]+)", src))
patched = set(int(m.group(1), 16) for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]{5,8})\s*,\s*0x[0-9A-Fa-f]{8}\s*\}", src))

import bisect
def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return starts[i] if i >= 0 else None
documented = {owner(a) for a in doc_hits} - {None}
replaced = {owner(a) for a in hooked | patched if in_text(a)} - {None}

imports = [l.strip() for l in open(P("decompiled", "macho", "imports.txt")) if l.strip()]
imp_done = [s for s in imports if re.search(r"\b_?" + re.escape(s.lstrip("_")) + r"\b", src)]

# ---------------------------------------------------------------- per-function status
# niveles: 0 sin decompilar, 1 decompilado con problemas, 2 decompilado limpio, 3 identificado (nombre o analizado),
#          4 reimplementado en el loader (hook/patch)
def level(a):
    if a in replaced:
        return 4
    q = fun_q.get(a)
    if a in sym or a in objc or a in documented:
        return 3 if q != "bad" else 1
    if q == "ok":
        return 2
    if q in ("warn", "bad"):
        return 1
    return 2 if (a in sym or a in objc) else 0
lv = {a: level(a) for a, _ in funcs}
by = collections.Counter(); bb = collections.Counter()
for a, n in funcs:
    by[lv[a]] += 1; bb[lv[a]] += n
NF, NB = len(funcs), sum(n for _, n in funcs)
pct = lambda x, t: 100.0 * x / t if t else 0

def bar(p, w=30):
    f = int(round(p * w / 100))
    return "█" * f + "░" * (w - f)

# library split by bytes (named funcs; unnamed inherit the library of the named neighbours when both agree)
named_lib = {a: lib_of(sym[a]) for a in sym if in_text(a)}
# ObjC methods are scattered among game code: they count as ObjC themselves but are not inherited by neighbours.
lib_bytes, lib_named = collections.Counter(), collections.Counter()
na = sorted(named_lib)
for a, n in funcs:
    if a in objc:
        L = "Objective-C (app/UI)"; lib_named[L] += n
    elif a in named_lib:
        L = named_lib[a]; lib_named[L] += n
    elif a in fun_tag:
        L = fun_tag[a]
    else:
        i = bisect.bisect_right(na, a)
        lo = named_lib[na[i - 1]] if i > 0 else None
        hi = named_lib[na[i]] if i < len(na) else None
        L = lo if lo == hi and lo else "Juego (sin símbolos, estimado)"
    lib_bytes[L] += n

# ---------------------------------------------------------------- map
CELL = 4096
ROW = 64
glyph = {0: "·", 1: "▒", 2: "▓", 3: "█", 4: "◆"}
cells = []
fi = 0
for c0 in range(T0, T1, CELL):
    c1 = min(c0 + CELL, T1)
    acc = collections.Counter()
    while fi < len(funcs) and funcs[fi][0] + funcs[fi][1] <= c0:
        fi += 1
    j = fi
    while j < len(funcs) and funcs[j][0] < c1:
        a, n = funcs[j]
        acc[lv[a]] += min(a + n, c1) - max(a, c0)
        j += 1
    cells.append(4 if acc[4] else max(acc, key=lambda k: (acc[k], k)) if acc else 0)
rows = []
for r in range(0, len(cells), ROW):
    rows.append(f"0x{T0 + r * CELL:06x}  " + "".join(glyph[c] for c in cells[r:r + ROW]))

# ---------------------------------------------------------------- write
decomp = by[1] + by[2] + by[3] + by[4]
decomp_b = bb[1] + bb[2] + bb[3] + bb[4]
clean = by[2] + by[3] + by[4]
clean_b = bb[2] + bb[3] + bb[4]
ident = by[3] + by[4]
ident_b = bb[3] + bb[4]
disasm_b = insn * 4
out = []
w = out.append
w("# Hero of Sparta II — Mapa de decompilación (`HOS2`)\n")
w(f"_Generado por `tools/decomp_progress.py` el {datetime.date.today()} — regenerar tras cada avance._\n")
w(f"Binario: iOS ARM Mach-O, `__TEXT,__text` = `0x{T0:06x}`–`0x{T1:06x}` (**{TSZ/1024/1024:.2f} MB**, "
  f"{NF:,} funciones detectadas).\n")
w("## Resumen\n")
w("| Etapa | Funciones | % funciones | % bytes | Progreso (bytes) |")
w("| :--- | ---: | ---: | ---: | :--- |")
rowfmt = lambda name, n, b: w(f"| {name} | {n:,} / {NF:,} | {pct(n, NF):.1f}% | {pct(b, NB):.1f}% | `{bar(pct(b, NB))}` |")
w(f"| 1. Desensamblado (objdump) | {NF:,} / {NF:,} | 100.0% | {pct(disasm_b, TSZ):.1f}% | `{bar(pct(disasm_b, TSZ))}` |")
rowfmt("2. Decompilado (Ghidra, cualquier calidad)", decomp, decomp_b)
rowfmt("3. Decompilado limpio (sin jump tables rotas ni datos inválidos)", clean, clean_b)
rowfmt("4. Identificado (símbolo, método ObjC o analizado en el port)", ident, ident_b)
rowfmt("5. Reimplementado en C (hook/patch del loader)", by[4], bb[4])
w(f"| 6. Recompilación *matching* (C que produce el mismo binario) | 0 / {NF:,} | 0.0% | 0.0% | `{bar(0)}` |")
w("")
w(f"- Ghidra produjo **{total_ghidra:,}** funciones ({named_ghidra:,} con nombre: métodos ObjC, símbolos C++ y thunks).")
w(f"- Calidad del pseudo-C: **{list(fun_q.values()).count('ok'):,}** limpias, "
  f"**{list(fun_q.values()).count('warn'):,}** con `switch` cuya jump table Ghidra no recuperó (o tipos que no convergen), "
  f"**{list(fun_q.values()).count('bad'):,}** con datos inválidos (`halt_baddata`).")
w(f"- Símbolos con nombre en `__text`: **{sum(1 for a in sym if in_text(a)):,}**; métodos ObjC: **{len(objc):,}**; "
  f"funciones con dirección citada en el código/notas del port: **{len(documented):,}**.")
w(f"- Capa de plataforma (no es código del juego): **{len(imp_done)} / {len(imports)}** imports de iOS/libc "
  f"reconocidos por nombre en `source/` (el loader resuelve 1,930 slots, 0 sin implementar).")
w("")
w("## Mapa de `__text`\n")
w(f"Cada carácter = {CELL // 1024} KB de código, {ROW} por fila (dirección de inicio a la izquierda). "
  "Muestra el estado predominante de esos bytes:\n")
w("| Símbolo | Estado |\n| :---: | :--- |")
w("| `·` | Solo desensamblado (sin función de Ghidra) |")
w("| `▒` | Decompilado con problemas (jump table no recuperada / `halt_baddata`) |")
w("| `▓` | Decompilado limpio, sin identificar (`FUN_xxxxxxxx`) |")
w("| `█` | Identificado: símbolo, método ObjC o analizado en el port |")
w("| `◆` | Contiene una función reimplementada / parcheada por el loader |")
w("\n```text")
out.extend(rows)
w("```\n")
w("## Reparto por biblioteca (bytes de `__text`)\n")
w("Las funciones sin nombre se asignan por las rutas de `assert()` que citan (uSTL, TinyXML, decodificadores de vox) "
  "o heredan la biblioteca de sus vecinas con nombre cuando ambas coinciden; el resto se cuenta como código del juego "
  "(motor `pig` de Gameloft + lógica de Hero of Sparta II).\n")
w("| Biblioteca | Bytes | % de `__text` | % con nombre |")
w("| :--- | ---: | ---: | ---: |")
for L, n in lib_bytes.most_common():
    w(f"| {L} | {n:,} | {pct(n, NB):.1f}% | {pct(lib_named[L], n):.1f}% |")
w("")
w("## Qué falta para decompilarlo por completo\n")
w(f"1. **Tipar y nombrar** las {by[2]:,} funciones limpias `FUN_xxxxxxxx` "
  "(el binario no trae símbolos del juego: solo vox, GLLive, uSTL, libstdc++ y ObjC).")
w(f"2. **Arreglar** las {by[1]:,} funciones con problemas: Ghidra no siguió la jump table de sus `switch` "
  "(`add pc, pc, rX, lsl #2` + tabla inline; se arregla marcando la tabla y re-analizando) o leyó datos como código.")
w("3. **Recrear structs/clases** (vtables en `__const`, p. ej. la malla `0x344848`) para que el pseudo-C sea legible.")
w("4. **Reemplazar** bibliotecas conocidas por su código fuente: uSTL y libstdc++ son públicas; vox y GLLive son de Gameloft.")
w("5. **Recompilar** por módulos (empezando por el más chico) y validar en consola o por diff de instrucciones.")
w("")
w("## Cómo se mide\n")
w("- *Función*: unión de los inicios `FUN_xxxxxxxx` de Ghidra, el índice de símbolos y los `imp` ObjC; "
  "su tamaño es la distancia hasta el siguiente inicio.")
w("- *Desensamblado*: instrucciones de `full_HOS2.md` dentro de `__text` × 4 bytes.")
w("- *Analizado en el port*: cualquier dirección `0x…` de `__text` citada en `source/` o en los `.md` del repo.")
w("- *Reimplementado*: direcciones de `__text` pasadas a `macho_hook()` o a tablas `{va, insn}` de parches en `source/`. "
  "No cuenta el reemplazo genérico de bloques VFP vectoriales (`macho.c`), que parchea por patrón de instrucción.")
open(P("docs", "DECOMP_PROGRESS.md"), "w").write("\n".join(out) + "\n")
print(f"docs/DECOMP_PROGRESS.md: {NF} funcs, decomp {pct(decomp_b, NB):.1f}%, clean {pct(clean_b, NB):.1f}%, "
      f"ident {pct(ident_b, NB):.1f}%, replaced {by[4]}")
