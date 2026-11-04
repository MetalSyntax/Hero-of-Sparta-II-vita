# Hero of Sparta II — Mapa de decompilación (`HOS2`)

_Generado por `tools/decomp_progress.py` el 2026-11-04 — regenerar tras cada avance._

Binario: iOS ARM Mach-O, `__TEXT,__text` = `0x002e30`–`0x32102c` (**3.12 MB**, 10,264 funciones detectadas).

## Resumen

| Etapa | Funciones | % funciones | % bytes | Progreso (bytes) |
| :--- | ---: | ---: | ---: | :--- |
| 1. Desensamblado (objdump) | 10,264 / 10,264 | 100.0% | 100.0% | `██████████████████████████████` |
| 2. Decompilado (Ghidra, cualquier calidad) | 10,264 / 10,264 | 100.0% | 100.0% | `██████████████████████████████` |
| 3. Decompilado limpio (sin jump tables rotas ni datos inválidos) | 10,102 / 10,264 | 98.4% | 97.8% | `█████████████████████████████░` |
| 4. Identificado (símbolo, método ObjC o analizado en el port) | 777 / 10,264 | 7.6% | 7.6% | `██░░░░░░░░░░░░░░░░░░░░░░░░░░░░` |
| 5. Reimplementado en C (hook/patch del loader) | 0 / 10,264 | 0.0% | 0.0% | `░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░` |
| 6. Recompilación *matching* (C que produce el mismo binario) | 0 / 10,264 | 0.0% | 0.0% | `░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░` |

- Ghidra produjo **10,452** funciones (927 con nombre: métodos ObjC, símbolos C++ y thunks).
- Calidad del pseudo-C: **9,360** limpias, **163** con `switch` cuya jump table Ghidra no recuperó (o tipos que no convergen), **2** con datos inválidos (`halt_baddata`).
- Símbolos con nombre en `__text`: **551**; métodos ObjC: **189**; funciones con dirección citada en el código/notas del port: **48**.
- Capa de plataforma (no es código del juego): **435 / 476** imports de iOS/libc reconocidos por nombre en `source/` (el loader resuelve 1,930 slots, 0 sin implementar).

## Mapa de `__text`

Cada carácter = 4 KB de código, 64 por fila (dirección de inicio a la izquierda). Muestra el estado predominante de esos bytes:

| Símbolo | Estado |
| :---: | :--- |
| `·` | Solo desensamblado (sin función de Ghidra) |
| `▒` | Decompilado con problemas (jump table no recuperada / `halt_baddata`) |
| `▓` | Decompilado limpio, sin identificar (`FUN_xxxxxxxx`) |
| `█` | Identificado: símbolo, método ObjC o analizado en el port |
| `◆` | Contiene una función reimplementada / parcheada por el loader |

```text
0x002e30  ▓▓▓▓▓▓▓▓▓▓▓▓▓████▓▓▓▓▓▓▓▓▓▓▓▓██████████████████▓▓▓▓▓▓▓▓▓▓▓▓▓▓███
0x042e30  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▒▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▒▓
0x082e30  ▓▓▓▓▓▓▓█▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
0x0c2e30  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
0x102e30  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
0x142e30  ▓▓▓▓▓▓▓▓▓▓▓▒▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓█▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
0x182e30  ▓▓▓▓▓▓▓▓▓▓▓████▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▒▓▓▓
0x1c2e30  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▒▓▓▓▓▓▓▓▓▓▓
0x202e30  ▓█▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓█▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
0x242e30  ▓▓▓▓████████▓▓▓▓▓▓▓▒▓▓▓▓▓▓▓▓▓▓██▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▒▓█
0x282e30  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓██▓▓▓▓▓▓▓▓▓▓▓▓█▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓█▓▓▓▓█▓█▓▓▓▓█▓
0x2c2e30  ▓▓▓▓▓▓▓▓█▓▓▓▓█▓▓█▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▒▒▒▓▓▓▓▓▓▓▓▓▓▓▒▓▓▓▓▓▓▓▓▓
0x302e30  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
```

## Reparto por biblioteca (bytes de `__text`)

Las funciones sin nombre se asignan por las rutas de `assert()` que citan (uSTL, TinyXML, decodificadores de vox) o heredan la biblioteca de sus vecinas con nombre cuando ambas coinciden; el resto se cuenta como código del juego (motor `pig` de Gameloft + lógica de Hero of Sparta II).

| Biblioteca | Bytes | % de `__text` | % con nombre |
| :--- | ---: | ---: | ---: |
| Juego (sin símbolos, estimado) | 2,792,660 | 85.4% | 0.0% |
| GLLive (online) | 173,656 | 5.3% | 4.9% |
| libstdc++ | 148,472 | 4.5% | 30.0% |
| vox (audio engine) | 80,436 | 2.5% | 4.0% |
| Objective-C (app/UI) | 58,452 | 1.8% | 100.0% |
| TinyXML | 10,464 | 0.3% | 0.0% |
| C++ (juego/otros) | 4,252 | 0.1% | 100.0% |
| uSTL | 624 | 0.0% | 0.0% |
| GLT math | 76 | 0.0% | 100.0% |
| C/otros | 24 | 0.0% | 100.0% |

## Qué falta para decompilarlo por completo

1. **Tipar y nombrar** las 9,325 funciones limpias `FUN_xxxxxxxx` (el binario no trae símbolos del juego: solo vox, GLLive, uSTL, libstdc++ y ObjC).
2. **Arreglar** las 162 funciones con problemas: Ghidra no siguió la jump table de sus `switch` (`add pc, pc, rX, lsl #2` + tabla inline; se arregla marcando la tabla y re-analizando) o leyó datos como código.
3. **Recrear structs/clases** (vtables en `__const`, p. ej. la malla `0x344848`) para que el pseudo-C sea legible.
4. **Reemplazar** bibliotecas conocidas por su código fuente: uSTL y libstdc++ son públicas; vox y GLLive son de Gameloft.
5. **Recompilar** por módulos (empezando por el más chico) y validar en consola o por diff de instrucciones.

## Cómo se mide

- *Función*: unión de los inicios `FUN_xxxxxxxx` de Ghidra, el índice de símbolos y los `imp` ObjC; su tamaño es la distancia hasta el siguiente inicio.
- *Desensamblado*: instrucciones de `full_HOS2.md` dentro de `__text` × 4 bytes.
- *Analizado en el port*: cualquier dirección `0x…` de `__text` citada en `source/` o en los `.md` del repo.
- *Reimplementado*: direcciones de `__text` pasadas a `macho_hook()` o a tablas `{va, insn}` de parches en `source/`. No cuenta el reemplazo genérico de bloques VFP vectoriales (`macho.c`), que parchea por patrón de instrucción.
