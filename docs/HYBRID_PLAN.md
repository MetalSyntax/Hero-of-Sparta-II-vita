# Plan de decompilación dirigida (híbrida) — Hero of Sparta II

**Objetivo:** mejorar el port de PS Vita y nada más. El loader Mach-O se queda; solo se reemplazan (con hooks en C)
las funciones del juego que dan un beneficio concreto en la Vita. No es una decompilación completa, ni para otras
plataformas, ni de preservación (para eso está el `datafile.zip` de `ux0_data/`).

**Reglas** (de `CLAUDE.md`, aplican a cada fase):
- Un cambio por corrida en la consola física, guiado por logs. El juego tiene que seguir jugable después de cada paso.
- Nada del juego en git: en el repo solo van direcciones, nombres que le pongamos y código C propio.
- ABI iOS: softfp (floats en registros enteros), `r9` se preserva en callbacks.
- El avance se mide con `python3 -I tools/decomp_progress.py` (columnas "Identificado" y "Reimplementado").

**Tamaño estimado:** 150–300 funciones, alrededor del 2–3% de `__text`.

---

## Fase 0 — Herramientas (prerrequisito de todo)

Hoy `macho_hook()` (`source/macho.c`) solo engancha por **nombre de símbolo**, y el código del juego no tiene
símbolos. Tampoco hay forma de llamar a la función original.

| Tarea | Entregable | Hecho cuando |
| :--- | :--- | :--- |
| 0.1 Hook por dirección | `macho_hook_addr(va, dst)`: mismo trampolín de 16 bytes, por dirección del Mach-O. | Un hook de prueba sobre una función conocida loguea y el juego sigue igual. |
| 0.2 Llamar a la original | `macho_hook_addr` devuelve un *thunk* con las instrucciones pisadas reubicadas + salto de vuelta (comprobar que esas instrucciones no sean relativas a `pc`; si lo son, rechazar el hook y loguearlo). | Hook "pasa-todo" (log + llamada a la original) sin cambio de comportamiento. |
| 0.3 Sonda de llamadas | `probe.c`: engancha una lista de direcciones candidatas con contadores. Con **SELECT+L**, vuelca al log qué candidatas se ejecutaron desde el último volcado. | Se puede responder "¿qué función corre cuando pulso X?" en una sola corrida. |
| 0.4 Tabla de nombres | `docs/hos2_names.txt` (`0xADDR nombre  # nota`) con lo que se identifique (también exportable de Ghidra). `decomp_progress.py` lo lee como "identificado". | El mapa sube a medida que se nombra. |

**Técnica de identificación** que se usa en todas las fases: partir de una pista estática (string, constante,
vtable, llamada GL o ObjC) → elegir 10–50 funciones candidatas en Ghidra → sonda 0.3 en la consola → acción en el
juego → ver cuáles se dispararon → confirmar en el pseudo-C → nombrar en `hos2_names.txt`.

---

## Fase 1 — Input nativo (prioridad máxima)

**Problema actual:** `source/input.c` simula toques en coordenadas fijas (`controls.txt`). Eso trae varios problemas:
- Los botones contextuales (agarrar o iniciar el remate con CIRCLE) se mueven o aparecen en otro sitio.
- El joystick es digital, con zona muerta de 40.
- L+R choca con saltar+atacar.
- Pulsar CROSS en un menú toca lo que haya en (420,260).

**Meta:** que los botones de la Vita escriban directamente en el estado de input del juego, igual que lo haría su
HUD táctil, y que el táctil siga funcionando para los menús.

| Paso | Cómo encontrarlo | Entregable |
| :--- | :--- | :--- |
| 1.1 Entrada de toques | IMP ObjC de `touchesBegan/Moved/Ended:withEvent:` de la vista EAGL (en `objc_classes.txt`), seguir hasta el dispatcher C++ del juego. | Nombrar el gestor de toques y la lista de controles del HUD. |
| 1.2 Objetos de control | El dispatcher recorre controles con un rect y un estado. Las posiciones medidas (joystick 69,250; ataque 395,234; salto 455,191; defensa 351,294; escudo ~435,24; pausa ~24,25) deberían aparecer como constantes o en los datos de `gui_levelN.level`. | Struct "control" (rect, id, pressed, eje) y la instancia global del HUD. |
| 1.3 Acciones | Sonda sobre los métodos que lee el jugador cada frame: pulsar atacar, saltar, defender y agarrar por separado. | `hos2_names.txt`: `Player::Attack`, `Jump`, `Defend`, `Grab/Finisher`, `Shield`, `Pause` (o el flag que lean). |
| 1.4 Escribir el estado | Hook en la actualización de input del juego (una vez por frame): marcar los flags del control a partir de `pad.buttons`, *antes* de que la lógica los lea. Preferir escribir flags a llamar acciones (respeta cooldowns y animaciones). | `source/hos2_input.c`. `input.c` deja de sintetizar toques de botones. |
| 1.5 Stick analógico | El control del joystick guarda un vector o ángulo con magnitud: escribirlo desde `lx/ly` con curva y zona muerta circular pequeña (~12), en lugar del toque falso con zona muerta 40. | Movimiento analógico real, sin el falso toque del run 42. |
| 1.6 Contextuales y QTE | El botón de agarrar o rematar y los *prompts* de cinemática son controles que se activan por contexto: mapear CIRCLE a su flag, y si hay QTE, a la secuencia que pida. | CIRCLE agarra siempre, esté donde esté el icono. |
| 1.7 Menús con botones | Opcional: D-pad + CROSS/CIRCLE navegan los menús (índice del ítem seleccionado), así no hay toques a ciegas. | Menús jugables sin tocar la pantalla. |
| 1.8 L+R | Con input nativo, L y R dejan de ser toques, así que mostrar/ocultar el HUD puede pasar a SELECT+START o a mantener L+R 1 s. | Se puede saltar+atacar con L+R. |

**Hecho cuando:**
- Todas las acciones de la tabla de controles del README funcionan con el HUD oculto.
- El stick es analógico.
- Ningún botón produce toques fantasma en los menús.
- Se mantiene `controls.txt` como plan B (bandera `NATIVE_INPUT=0`).

**Riesgo:** medio. El hook corre en el hilo del juego; un flag mal escrito solo afecta al input.

---

## Fase 2 — HUD: ocultar los controles de verdad

**Problema actual:** `gles.c` oculta los controles a nivel GL. Aprende la textura del HUD y colapsa quads por zona
de pantalla. Es frágil: puede ocultar el *prompt* de agarrar si aparece encima del botón de ataque.

| Paso | Cómo encontrarlo | Entregable |
| :--- | :--- | :--- |
| 2.1 Dibujo del HUD | Desde la instancia del HUD de 1.2: su método `draw` llama al batcher 2D (el de los quads, buffer de índices alloc 0x18D7D4). | Nombrar `HUD::Draw` y el dibujo por control. |
| 2.2 Opacidad por control | Hook en el dibujo de cada control: si está oculto, no dibujar, o pasar alpha 0 como hace Zombie Infection. Excluir los *prompts* contextuales (agarrar o rematar), la vida, la pausa y el escudo. | Se elimina `hud_filter()` de `gles.c`. |

**Hecho cuando:** los controles se ocultan con 0% en todos los niveles, los *prompts* siguen visibles y el log no
muestra `[HUD] learned`.

---

## Fase 3 — Partidas guardadas ("Continuar" no avanza)

**Pistas ya ubicadas** (análisis estático, `port_progress.md`):

| Dirección | Qué hace |
| :--- | :--- |
| `0x7a9a0` | Guardado de checkpoint: escribe `statisticsN`, `saveachievements` y `savegame`. |
| `0x7ae80` | Restaura el checkpoint **solo si** el nivel y el número de entidades guardados coinciden con los actuales. |
| `0x7a640` | Carga las estadísticas; si la cabecera (0,1,4) no coincide, **borra** `savegame`/`savebackup` (`0x7a5e0`). |
| `0x7e5xx` | "Continuar": lee el nivel desde `savegame` si `DAT_0034a070 == -1`. |
| `0x81190` | Borra el guardado tras el diálogo `0x16bcc0` (¿nueva partida?). |

| Paso | Entregable |
| :--- | :--- |
| 3.1 Leer las líneas `[SAVE]` de una corrida: checkpoint → salir → Continuar. | Saber si el fallo está en escribir, en leer, en el borrado o en el chequeo de entidades. |
| 3.2 Si es el chequeo de entidades (`0x7ae80`): hook que loguee el conteo guardado frente al actual. | Causa exacta. |
| 3.3 Arreglo mínimo en la función culpable. | "Continuar" vuelve al último checkpoint. |
| 3.4 Opcional: autoguardado al suspender la Vita (PS / LiveArea). `applicationWillResignActive` solo pausa; llamar a `0x7a9a0` si estás en juego. | No se pierde progreso al cerrar la app. |

---

## Fase 4 — Memoria: quitar la cuarentena

**Problema actual:** el juego dibuja mallas desde arrays que ya liberó (destructores `ustl::memblock` 0x18BBF8 /
0x18BD70). `darwin.c` retiene hasta 136 MB de memoria liberada para compensarlo, lo que deja poco RAM libre y hace
de cada stage grande un riesgo.

| Paso | Cómo encontrarlo | Entregable |
| :--- | :--- | :--- |
| 4.1 Quién destruye el vector | Los bloques fijados (`[MEM] pinned ... freed by`) llevan el `lr`. Agregar a ese log el `lr` del llamador del destructor (un nivel más arriba). | La función de la malla (vtable `0x344848`, render `0x200ED0`) que suelta sus arrays y sigue dibujando. |
| 4.2 Arreglo en origen | Hook: que esa malla no libere los arrays mientras exista, o que el render use la copia viva. | Sin `[MEM] pinned` en una partida completa. |
| 4.3 Bajar la cuarentena | Volver a la cuarentena de 16–32 MB solo como red de seguridad, o quitarla. | Más de 100 MB de RAM libres para la fase 5. |

---

## Fase 5 — Imagen: Retina y panorámico real

**Pista fuerte:** el IPA trae assets Retina (`gameTextures@2x`, `models@2x`, `anims@2x`, `clev@2x`,
`states_game@2x`). El port le responde al juego `contentScaleFactor = 1.0` (`objc.c`) y un renderbuffer de 320×480.

| Paso | Entregable |
| :--- | :--- |
| 5.1 Ver cómo elige el juego `@2x` (`contentScaleFactor` / `[UIScreen scale]`, tamaño del renderbuffer). | Nombrar la función de selección de assets. |
| 5.2 Responder escala 2.0 y un renderbuffer de 640×960; ajustar `gles.c` (FBO) e `input.c` (×2 en los toques). | El juego renderiza a 960×640 con texturas Retina, y se reduce a 544 de alto: más nitidez. **Requiere la Fase 4** por la memoria extra de texturas. |
| 5.3 Panorámico real: ubicar el cálculo del FOV y el aspecto de la cámara (`glFrustum`/`glMultMatrixf` antes de cada escena 3D) y el anclaje del HUD a los bordes. | El HUD se pega a los bordes de 16:9; se elimina el truco `WIDE_K` de `gles.c`. |

**Riesgo:** alto (memoria, rendimiento). Hacerlo después de las fases 1–4 y medir FPS en cada paso.

---

## Fase 6 — Rendimiento (solo si hace falta tras la fase 5)

| Paso | Entregable |
| :--- | :--- |
| 6.1 Contador de FPS y tiempo de frame en el log, de forma permanente (cada 5 s, como en Skater Nation). | Línea base por nivel. |
| 6.2 Sonda 0.3 con tiempos (no solo conteos) sobre las funciones grandes del render y la animación. | Top 10 de funciones calientes. |
| 6.3 Reescribir en C con NEON y hardfp las 2–5 más calientes (matemática de matrices `GLTmatrix`, skinning, culling). | +FPS medido en el mismo punto del juego. |

---

## Orden y cortes de release

| Release | Contenido | Corridas aprox. |
| :--- | :--- | ---: |
| **v1.0** | Lo actual (pantalla completa, HUD por GL, mapeo por toques, cuarentena) | — |
| **v1.1** | Fases 0, 1 y 3: input nativo, CIRCLE para agarrar, "Continuar" arreglado | 8–15 |
| **v1.2** | Fases 2 y 4: HUD limpio, sin cuarentena, más RAM | 5–10 |
| **v1.3** | Fase 5: Retina + panorámico real | 6–12 |
| v1.4 | Fase 6, si los FPS lo piden | variable |

## Seguimiento

- Cada función identificada va a `docs/hos2_names.txt` y cada hook a `source/hos2_*.c`.
- `tools/decomp_progress.py` regenera `docs/DECOMP_PROGRESS.md`. La meta de este plan es llegar a ~10% en
  "Identificado" en las zonas del input, HUD, guardado, mallas y cámara, y a ~1–2% en "Reimplementado", no al 100%.
- Cada corrida se registra en `port_progress.md` con su log, igual que hasta ahora.
