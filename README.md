# VertexAV Link

Protocolo abierto para que el software de DJ comparta en tiempo real lo que sabe de la música
(tempo con hora exacta, decks, mezcla, estructura del tema, cuentas atrás, intención) con
software de luces y visuales. **VertexAV describe la música; no da órdenes.**

La especificación está en [spec/vertexav-link-spec.md](spec/vertexav-link-spec.md) y el plan de
trabajo en [spec/fases.md](spec/fases.md). Lo que se aprendió al implementarla (huecos,
ambigüedades y propuestas para la 0.7) está en
[spec/notas-implementacion-0.6.md](spec/notas-implementacion-0.6.md).

## Contenido

| Carpeta | Qué hay | Fase |
|---|---|---|
| [spec/](spec) | Especificación 0.6, plan y notas de implementación | — |
| [tools/](tools) | Emisor simulado, inspector (receptor que valida todo), pasarela a OS2L y servidor OS2L de pruebas, en Python | 1 y 2 |
| [libvertexav/](libvertexav) | Librería de referencia en C++/Qt (emisor, receptor, WebSocket, mDNS), sin dependencias más allá de QtCore y QtNetwork | 3 |
| [integrations/qlcplus/](integrations/qlcplus) | Plugin de entrada para QLC+: tempo sin tap, secciones, colores, disparos | 4 |
| [integrations/mixxx/](integrations/mixxx) | Emisor dentro de Mixxx (rama `vertexav-link` del fork) | 5 |

## Estado

| Fase | Estado | Cómo se probó |
|---|---|---|
| 1. Emisor de pruebas + inspector | Hecho: perfiles 0–2, modos `open` y `auth`, DNS-SD, límites de §15.7 | 57 pruebas automáticas (incluye validar todos los ejemplos JSON de la especificación) y sesiones de varios minutos sin errores |
| 2. Pasarela a OS2L | Hecho | Servidor OS2L de pruebas: error de llegada del beat ≈ 1 ms de mediana |
| 3. libvertexav | Hecho (perfiles 0–2) | 15 pruebas con Qt 6 y Qt 5.15; interoperabilidad en ambos sentidos con las herramientas Python, también en modo `auth`; DNS-SD en ambos sentidos |
| 4. Plugin de QLC+ | Hecho | Compila contra el código de QLC+ (Qt 5 y 6); arnés que lo carga como QLC+ y comprueba beats, secciones, paleta y disparos |
| 5. Mixxx | Implementado en la rama `vertexav-link` del fork | Ver [integrations/mixxx](integrations/mixxx). Falta la prueba con un equipo de DJ real |

El perfil 3 (características de audio y PCM) no está implementado todavía en ninguna parte.

## Prueba rápida (mismo PC)

```sh
cd tools
pip install websockets zeroconf
python -m vertexav.sim --fast            # emisor simulado: dos decks, mezclas, drops, loops, scratch
python -m vertexav.inspector              # en otra terminal: muestra y valida todo
python -m vertexav.os2l                   # pasarela a OS2L (QLC+ escucha en el puerto 9996)
```

Con seguridad:

```sh
python -m vertexav.sim --gen-psk                         # genera un código de emparejamiento
python -m vertexav.sim --security auth --psk CODIGO
python -m vertexav.inspector --psk CODIGO
```

Pruebas automáticas:

```sh
cd tools && python -m pytest                       # Python
cmake -S libvertexav -B build && cmake --build build && build/vertexav_tests   # C++
```

## Licencia

El repositorio está bajo Apache-2.0 (archivo [LICENSE](LICENSE)). El plan
([spec/fases.md](spec/fases.md)) propone MIT para la librería; ver la cuestión abierta 6 de la
especificación.
