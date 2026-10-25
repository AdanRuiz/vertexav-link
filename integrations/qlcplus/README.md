# Plugin VertexAV Link para QLC+

Plugin de entrada que conecta QLC+ con un emisor VertexAV Link (Mixxx con el emisor, el
simulador `vertexav.sim` o `vxemit`). Cubre la fase 4 del plan:

- **Tempo sin tap:** cada beat llega con su hora exacta y alimenta el generador de beats de
  QLC+. Los chasers y EFX en modo de tempo *Beats* siguen al DJ sin pulsar *Tap*.
- **Bancos de escenas por sección:** un canal por rol de sección (`intro`, `build`, `drop`…)
  encendido mientras el tema en el aire está en esa sección.
- **Colores por intención:** la paleta sugerida (hasta 6 colores) como canales R, G, B, más
  energía, ambiente, intensidad y `strobe_ok`.
- Cuentas atrás antes de los drops, estado de la mezcla y disparos del DJ.

Usa [libvertexav](../../libvertexav) en un hilo propio, así que la sincronización de reloj no
depende de la carga de la interfaz.

## Compilar dentro de QLC+

```sh
integrations/qlcplus/install-into-qlcplus.sh /ruta/a/qlcplus
cd /ruta/a/qlcplus && cmake -S . -B build && cmake --build build
```

El script copia el plugin y libvertexav a `qlcplus/plugins/vertexav` y añade
`add_subdirectory(vertexav)` a `plugins/CMakeLists.txt`. Funciona con Qt 6 (QLC+ 5) y Qt 5.15
(QLC+ 4). No añade dependencias: solo QtCore, QtNetwork, QtGui y QtWidgets.

## Compilar suelto y probarlo sin QLC+

```sh
integrations/qlcplus/build-standalone.sh /ruta/a/qlcplus ~/qlcbuild
integrations/qlcplus/test/run-harness.sh ~/qlcbuild python3 35
```

El arnés de pruebas carga el plugin con `QPluginLoader`, como QLC+, lo conecta al simulador y
muestra qué canales se movieron y la regularidad de los beats.

## Uso en QLC+

1. **Entradas/Salidas:** en un universo, marca la entrada *VertexAV Link*. Con el botón de
   configuración puedes elegir el emisor (vacío = el primero que se encuentre en la red), el
   código de emparejamiento si el emisor usa el modo `auth`, y la **latencia de salida**: el
   retraso de tu interfaz DMX y los aparatos. El plugin dispara los beats y eventos ese tiempo
   antes para que la luz cambie justo a tiempo.
2. **Tempo sin tap:** en la configuración de Entradas/Salidas, generador de beats = *Plugin*.
   En los chasers y EFX, elige el tipo de tempo *Beats*.
3. **Escenas por sección:** en la consola virtual, crea un *Solo Frame* con un botón por
   sección y asigna a cada uno su canal `section.*` (o usa el asistente de detección
   automática mientras suena el simulador). Cada botón lanza la escena o colección de esa
   sección.
4. **Colores:** asigna los canales `palette.N.r/g/b` a deslizadores en modo *Level* que
   controlen los canales RGB de tus aparatos.

## Canales de entrada

| Canal | Nombre | Valor |
|---|---|---|
| 1 | `beat` | Pulso 255 en cada beat (también generador de beats de QLC+) |
| 2 | `downbeat` | Pulso en el "1" de cada compás |
| 3 | `tempo.bpm` | BPM − 50 (0–255) |
| 4 | `tempo.playing` | 255 mientras suena música con tempo |
| 5 | `energy` | Energía 0–255 |
| 6 | `transition.progress` | Avance de la mezcla 0–255 |
| 7 | `transition.active` | 255 durante una mezcla |
| 8 | `mix.cut` | Pulso en un corte rápido entre decks |
| 10–19 | `section.intro`, `verse`, `chorus`, `build`, `drop`, `breakdown`, `bridge`, `outro`, `silence`, `unknown` | 255 en la sección actual del tema en el aire |
| 20–24 | `countdown.16`, `8`, `4`, `2`, `1` | Pulso N beats antes de un drop |
| 30 | `strobe_ok` | 255 si es buen momento para efectos agresivos |
| 31–34 | `intensity.low`, `medium`, `high`, `peak` | Uno a 255 |
| 40–49 | `mood.dark`, `euphoric`, `happy`, `melancholic`, `aggressive`, `chill`, `dreamy`, `groovy`, `epic`, `neutral` | Uno a 255 |
| 50–67 | `palette.1.r` … `palette.6.b` | Colores sugeridos (3 canales por color) |
| 70–73 | `deck.1.audible` … `deck.4.audible` | Cuánto se oye cada deck |
| 74–77 | `deck.N.playing` | 255 si el deck suena |
| 78–81 | `on_air.N` | 255 en el deck en el aire |
| 100–107 | `blackout`, `fullon`, `strobe`, `freeze`, `fog`, `mode.chill`, `mode.build`, `mode.peak` | Disparos del DJ |
| 110–117 | `color.red`, `green`, `blue`, `white`, `amber`, `purple`, `cyan`, `pink` | Disparos de color |
| 120–135 | `fx.1` … `fx.16` | Efectos genéricos |
| 200–999 | `custom.*` y otros | 200 + CRC16(nombre) mod 800; usa el asistente de detección |

Los disparos `momentary` y los pulsos duran 60 ms. Un disparo con `value` usa ese valor
(0–1 → 1–255).
