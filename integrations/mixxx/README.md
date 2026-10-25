# Emisor VertexAV Link en Mixxx (fase 5)

El código está en el fork de Mixxx, rama **`vertexav-link`** (sin commits todavía: los cambios
están en el árbol de trabajo para que los revises). Archivos:

| Archivo | Qué hace |
|---|---|
| `lib/vertexav/` | Copia de [libvertexav](../../libvertexav) (`sync-into-mixxx.sh` la actualiza) |
| `src/vertexav/vertexavmanager.{h,cpp}` | Lee los decks, calcula tempo, mezcla y eventos, y publica con el emisor en su propio hilo |
| `src/vertexav/vertexavtrackinfo.{h,cpp}` | Metadatos, beatgrid, marcadores y secciones de un tema a partir de la biblioteca |
| `src/preferences/dialog/dlgprefvertexav.{h,cpp}` | Página *Preferencias → VertexAV Link* |
| `CMakeLists.txt` | Opción `VERTEXAV` (activada por defecto) |
| `src/coreservices.*`, `src/preferences/dialog/dlgpreferences.cpp` | Crear el gestor y añadir la página |

## Compilar

Como cualquier Mixxx (ver `CONTRIBUTING.md` del proyecto). La opción `-DVERTEXAV=OFF` lo deja
fuera. No hay dependencias nuevas: libvertexav solo usa QtCore y QtNetwork.

Para actualizar la librería después de cambiarla aquí:

```sh
integrations/mixxx/sync-into-mixxx.sh /ruta/a/mixxx
```

## Usar

*Preferencias → VertexAV Link*: activar, nombre, puertos (9470/9471), modo de seguridad
(`open` o `auth` con código de emparejamiento), antelación de eventos y un desfase de salida para
compensar el retraso del equipo de sonido. Al activarlo, Mixxx se anuncia en la red y
QLC+ (con el plugin), la pasarela OS2L o el inspector lo encuentran solos.

Controles nuevos, para mapear en controladores, teclado o skins:

| Control | Uso |
|---|---|
| `[VertexAV],enabled` | Activar o desactivar el emisor (se guarda) |
| `[VertexAV],receivers` | Número de receptores conectados (solo lectura) |
| `[VertexAV],trigger_strobe`, `trigger_blackout`, `trigger_fullon`, `trigger_freeze`, `trigger_fog` | Disparos manuales del DJ (pulsado = `on`, soltado = `off`) |
| `[VertexAV],trigger_mode_chill`, `trigger_mode_build`, `trigger_mode_peak` | Modos |
| `[VertexAV],trigger_color_red`, `_green`, `_blue`, `_white`, `_amber`, `_purple` | Colores |
| `[VertexAV],trigger_fx1` … `trigger_fx8` | Efectos genéricos `fx.1` … `fx.8` |

## Qué publica y de dónde sale

| VertexAV | Origen en Mixxx |
|---|---|
| `tempo` (perfil 0) | BPM efectivo y posición del deck en el aire, tomados de `VisualPlayPosition`: la posición del sample que está saliendo por la tarjeta de sonido **ahora**, no la que se acaba de procesar. Se vuelve a anclar si el BPM cambia o la fase deriva más de 3 ms |
| `stability` | Scratch (`scratch2_enable`) = `unstable`; con Slip Mode = `freewheel` con la posición de slip; espera 1 beat tras el scratch antes de volver a `stable` (§9.4) |
| `phase_reset` | Saltos de cue, búsquedas y cambios de deck con otra fase. Los beatjump y loops que conservan la fase no lo activan |
| `deck.state` | `play`, posición, BPM, `volume`, crossfader y orientación (`audible`), EQ (`EqualizerRack1`) y kills, filtro (`QuickEffectRack1 super1`), `pfl`, loop, scratch, slip |
| `mix.state` y eventos de mezcla | `MixAnalyzer` de libvertexav (transiciones, cortes, gates, deck en el aire) |
| `track.loaded` | Biblioteca: título, artista, álbum, género, año, duración, BPM, tonalidad (notación Lancelot = Camelot), color, puntuación, comentario |
| `track.structure` | Beatgrid (constante o variable), hotcues, intro/outro y loops guardados. **Secciones**: a partir de las etiquetas de los hotcues (`Drop`, `Build`, `Breakdown`, `Intro`, `Outro`, `Chorus`/`Estribillo`, `Verse`/`Verso`, `Bridge`/`Puente`) y de los marcadores de intro y outro. Es la fuente de mayor prioridad según §11.2 |
| `section.change` y `countdown` | Del mapa del deck en el aire, anunciados con antelación y cancelados si un salto o un loop los hace imposibles |
| `intent` | Energía según el rol de la sección, ambiente según el campo *mood* o el género, paleta a partir del color del tema |

Mixxx no detecta secciones por sí solo: sin hotcues etiquetados, el mapa solo tiene intro/outro
(si están marcados) y una sección `unknown`. Etiquetar el hotcue del drop como "Drop" basta para
tener cuenta atrás y cambio de sección exactos.

## Hilos y tiempo real

- El hilo de audio no se toca. El gestor corre en el hilo principal cada 10 ms y solo lee
  controles (`PollingControlProxy`) y `VisualPlayPosition`.
- El emisor (red, JSON, `clock.pong`) corre en su propio `QThread`, así que la carga de la
  interfaz no retrasa las respuestas de reloj.

## Pruebas

1. Compilar con `-DVERTEXAV=ON`, activar en Preferencias y comprobar el registro:
   `VertexAV: emitter vx-... listening on port 9470`.
2. En otro equipo o terminal: `python -m vertexav.inspector --discover`. Debe mostrar
   `welcome`, `snapshot`, `track.loaded`, `track.structure`, `tempo` y paquetes UDP, sin errores.
3. Cargar un tema con un hotcue etiquetado "Drop", reproducir y ver la cuenta atrás y el
   `section.change` en el inspector.
4. Mezclar con faders y con corte de crossfader: `transition.start/end`, `mix.cut`,
   `on_air.change`.
5. Scratch con y sin Slip Mode: `stability` `unstable`/`freewheel` y vuelta a `stable`.

Según las normas del proyecto Mixxx (AGENTS.md), antes de proponer estos cambios hace falta
probarlos con un equipo de DJ real, y el commit y el pull request deben hacerlos personas.
