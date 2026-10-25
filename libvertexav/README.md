# libvertexav

Implementación de referencia de VertexAV Link 0.6 en C++17 y Qt (Qt 6, o Qt 5.15). Solo
depende de QtCore y QtNetwork: incluye su propio WebSocket (RFC 6455, solo texto) y un
respondedor/navegador mDNS mínimo, para que Mixxx y QLC+ no necesiten dependencias nuevas.

| Clase | Para qué |
|---|---|
| `vertexav::Emitter` | Emisor: WebSocket + UDP + DNS-SD, sesiones, `open`/`auth`, límites de §15.7, snapshot y deltas con las reglas de frecuencia de §8.4, tempo UDP a 20 Hz más un paquete por beat con antelación. Los métodos de publicación se pueden llamar desde cualquier hilo. |
| `vertexav::Receiver` | Receptor: reconexión, autenticación mutua, sincronización de reloj por UDP, validación de paquetes (§8.3), estado, eventos disparados en su instante y señal `beat()` en cada beat menos la latencia propia. |
| `vertexav::MixAnalyzer` | Deck en el aire, transiciones, cortes y gates (§10.2), igual que `tools/vertexav/mixlogic.py`. |
| `vertexav::Tempo`, `TempoFollower`, `ClockSync` | Modelo de tempo, suavizado del receptor (§9.4) y filtro de reloj (§7.2). |
| `vertexav::MdnsResponder`, `MdnsBrowser` | Anuncio y búsqueda de `_vertexav._tcp`. |
| `vertexav::security` | Códigos de emparejamiento, HMAC, HKDF, firma UDP. |

## Compilar y probar

```sh
cmake -S . -B build -G Ninja
cmake --build build
build/vertexav_tests
```

Opciones: `-DVERTEXAV_QT_MAJOR=5` para Qt 5, `-DVERTEXAV_BUILD_TESTS=OFF`,
`-DVERTEXAV_BUILD_TOOLS=OFF`.

Herramientas incluidas:

- `vxemit`: emisor de pruebas con una sesión de dos decks (para validarlo con el inspector
  Python).
- `vxdump`: receptor que imprime mensajes, beats y eventos, y mide la precisión de la señal de
  beat.

Scripts de interoperabilidad (Linux/WSL):

- `scripts/interop_cpp_receiver.sh <build> <python>`: `vxdump` contra el simulador Python en
  modo `auth`.
- `scripts/test-mdns.sh <build> <python>`: descubrimiento DNS-SD en ambos sentidos.
- `scripts/build-qt5.sh <qlcplus>`: compila la librería, sus pruebas y el plugin de QLC+ con Qt 5.

## Uso mínimo

```cpp
vertexav::EmitterConfig cfg;
cfg.name = "Mi software";
auto* emitter = new vertexav::Emitter(cfg);   // mejor en un QThread propio
emitter->start();

vertexav::Tempo t;
t.bpm = 128; t.anchorT = emitter->now(); t.anchorBeat = 0; t.playing = true;
emitter->setTempo(t);
emitter->emitEvent("section.change", emitter->now() + 150000, 1,
               {{"from", "build"}, {"to", "drop"}, {"section_index", 2}, {"energy", 0.95}});
```

```cpp
vertexav::ReceiverConfig rc;
rc.latencyUs = 25000;                       // retraso de la interfaz DMX
auto* rx = new vertexav::Receiver(rc);
QObject::connect(rx, &vertexav::Receiver::beat, [](qint64 i, int inBar, double bpm, bool) { /* luz */ });
QObject::connect(rx, &vertexav::Receiver::sectionChanged, [](int, const QString& role, double) { /* escena */ });
rx->start();
```
