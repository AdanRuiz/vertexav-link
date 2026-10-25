# Herramientas de pruebas (fases 1 y 2)

Paquete Python `vertexav` (Python 3.10+, `websockets` y `zeroconf`).

```sh
pip install -e .[test]     # o: pip install websockets zeroconf pytest pytest-asyncio
```

## Emisor simulado: `python -m vertexav.sim`

Se anuncia por DNS-SD (`_vertexav._tcp`), abre los puertos 9470 (WebSocket) y 9471 (UDP) y
simula una sesión sin fin con dos decks:

- temas con intro, build, drop, breakdown, build, drop y outro, con su mapa (perfil 2);
- `section.change` y cuentas atrás (16, 8, 4, 2, 1 beats) anunciados con `lookahead_us` de
  antelación;
- mezclas alternas: fundido con faders y corte de crossfader con un "chop";
- el segundo build se repite en loop justo antes del drop, lo que obliga a enviar
  `event.cancel`;
- scratch con Slip Mode (`freewheel`) y sin él (`unstable` y luego `phase_reset`);
- cambio gradual de tempo tras cada mezcla.

Opciones útiles: `--fast` (secciones cortas), `--start-beat N`, `--host 127.0.0.1` (solo este
equipo), `--security auth --psk CODIGO`, `--no-announce`, `--duration S`.

## Inspector: `python -m vertexav.inspector`

Receptor de pruebas. Se conecta (o busca con `--discover`), sincroniza el reloj por UDP y
muestra cada mensaje con una línea de resumen. Cada mensaje se valida contra la especificación:

- campos obligatorios, tipos, rangos y enumeraciones de cada tipo de mensaje;
- reglas de sesión: `seq` creciente, deltas antiguos, temas no suscritos, mensajes antes del
  snapshot, eventos previsibles sin antelación, `event.cancel` de eventos desconocidos o tardíos,
  cuentas atrás coherentes con el tempo, `section.change` coherente con el mapa, límites de
  frecuencia de §8.4, transiciones atascadas;
- paquetes UDP: orden de validación de §8.3, ventanas de `seq`, coherencia de `next_beat_t` y
  `next_downbeat_t`, tempo UDP frente a tempo JSON, frecuencia mínima de 20 Hz.

Al terminar (Ctrl+C o `--duration`) imprime un informe: pérdidas, reloj, latencia de entrega,
precisión del beat y la lista de problemas. `--strict` devuelve código 1 si hubo errores;
`--log sesion.jsonl` graba la sesión en JSON Lines.

## Pasarela OS2L: `python -m vertexav.os2l`

Receptor VertexAV y cliente OS2L (por defecto QLC+ en `127.0.0.1:9996`, o `--os2l-discover`).
Envía `beat` en el instante exacto de cada beat (`--latency-ms` para adelantarlo según el
retraso de la interfaz DMX), `btn` por cada disparo y `vertexav.section.<rol>` al cambiar de
sección. Con `--no-extras` envía solo lo de §16.1.

`python -m vertexav.os2l_mock --check` hace de QLC+: muestra lo que llega y mide el error de
llegada de cada beat frente al instante que publicó el emisor.

## Pruebas

```sh
python -m pytest
```

- `tests/test_core.py`: formato binario, ventanas de `seq`, HKDF (vector del RFC 5869),
  seguidor de tempo, limitación de frecuencia, análisis de mezcla y validación de **todos los
  ejemplos JSON de la especificación**.
- `tests/test_integration.py`: sesiones reales en localhost: `open`, `auth`, clave incorrecta,
  rechazo de rebajas de seguridad, detección de bucles, límites antes de autenticar, versión
  incompatible y una sesión simulada validada mensaje a mensaje.
