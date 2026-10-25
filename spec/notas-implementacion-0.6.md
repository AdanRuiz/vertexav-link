# Notas de implementación sobre el borrador 0.6

Huecos y ambigüedades encontrados al programar el emisor de pruebas, el inspector, la pasarela
OS2L y libvertexav. Cada punto indica qué hacen hoy las implementaciones de este repositorio
y una propuesta de texto para la versión 0.7.

## Sesión y control

1. **Formato de `snapshot` (§8.2, Anexo A).** La especificación dice qué contiene pero no su forma.
   Implementado:
   ```json
   {"type":"snapshot","t":50100000,
    "tempo":{...campos de tempo...},
    "decks":[{...deck.state completo...}],
    "mix":{...mix.state completo...},
    "tracks":[{...track.loaded sin type...}],
    "structures":[{...track.structure sin type...}],
    "intent":{...} | null,
    "events":[{...event pendiente (at futuro)...}]}
   ```
   Cada elemento tiene los mismos campos que su mensaje, sin `type`. Así un receptor reutiliza
   sus manejadores. `events` permite que un receptor que se conecta a mitad de una cuenta atrás
   no pierda los eventos ya anunciados.
2. **Snapshot sin `subscribe` (§6.4).** "Si no envía subscribe, recibe todo lo de sus perfiles",
   pero no se dice cuándo llega el snapshot. Implementado: el emisor lo envía al recibir
   `subscribe`, o 1 s después de `welcome` si no llega ninguno. Un `subscribe` posterior provoca
   un snapshot nuevo.
3. **`realtime_over_control` (§5.3).** No se define cómo llega el tempo de tiempo real por
   WebSocket. Implementado: mensaje `tempo.rt` con los campos del paquete binario
   (`bpm`, `anchor_t`, `anchor_beat`, `next_beat_t`, `next_downbeat_t`, `playing`, `stability`,
   `phase_reset`) a la misma frecuencia que el UDP.
4. **Cupo de receptores (§15.7).** No hay código de error para "emisor lleno". Implementado:
   `internal` con `fatal: true`. Propuesta: nuevo código `capacity` (fatal).
5. **`trigger.at`.** Aparece en el ejemplo de §10.5 pero no en la tabla de campos. Propuesta:
   añadirlo ("instante en que se dispara; igual a `t` si es inmediato").
6. **Posición de los decks en deltas (§10.1).** `position_s` es válida en el instante `t` del
   mensaje, pero un delta posterior sin posición (por ejemplo, solo `audible`) cambia `t`.
   El receptor debe guardar el `t` del último mensaje que traía posición. Propuesta: decirlo
   explícitamente.
7. **Numeración de beats de `tempo` (§9.2).** El ejemplo usa `anchor_beat: 1024` y
   `position_beat: 196` en el mismo instante. Implementado: `anchor_beat` es un contador
   continuo del emisor (línea de tiempo maestra) que no salta al cambiar de deck en el aire, y
   `bar_offset` alinea sus compases con los del deck. Propuesta: decirlo en §9.1.
8. **`transition.end` repite `deck`** arriba y en `data`. Inofensivo; propuesta: quedarse con uno.
9. **Cambio de deck en el aire y sección actual.** Tras `on_air.change` no se envía
   `section.change`, así que un receptor que solo mira eventos no sabe en qué sección está el
   nuevo deck. La pasarela OS2L lo calcula con el mapa y la posición. Propuesta: el emisor
   **DEBERÍA** enviar `section.change` (con `from` = rol anterior del deck saliente) al cambiar
   de deck en el aire.

## Tiempo real (UDP)

10. **Qué `mac_key` usa el emisor para un `clock.ping` (§15.5).** `session_key` es el mismo
    para todos los receptores, así que no identifica al receptor. Implementado: el emisor
    identifica la sesión por (IP, puerto) = la IP del WebSocket y el `udp_port` del `hello`; el
    receptor **DEBE** enviar los ping desde ese mismo socket. Propuesta: decirlo en §7.2.
11. **`phase_reset` en UDP.** Si se pierde el único paquete con el bit, el receptor suavizaría
    un salto. Implementado: el emisor mantiene el bit durante 250 ms tras el salto, y el
    receptor trata como salto cualquier diferencia mayor de ¼ de beat (§9.4).
12. **`bar_offset` no viaja en el paquete binario.** El receptor lo deduce de `next_downbeat_t`.
    Funciona; propuesta: mencionarlo.
13. **Paso 6 de validación (§8.3)** ("`t` razonable") no se puede aplicar antes de la primera
    estimación del reloj. Implementado: se aplica en cuanto hay una muestra de reloj.
14. **"8 paquetes seguidos" (§8.3).** Implementado como 8 valores de `seq` consecutivos desde el
    nuevo punto, aunque entre medias lleguen paquetes normales.
15. **Filtro de `rtt` (§7.2).** En el mismo equipo `rtt_min` es de unos 100 µs y 1,5 × `rtt_min`
    descarta casi todas las muestras por el ruido del planificador. Implementado: umbral
    `max(1,5 × rtt_min, rtt_min + 200 µs)`. Propuesta: añadir ese mínimo absoluto.
16. **Paquete tempo cuando no suena nada.** "Al menos 20 Hz mientras haya música". Implementado:
    2 Hz con `playing = 0` cuando está parado, para que el receptor sepa que el emisor sigue ahí.

## Mezcla (§10.2)

17. **`mix.cut`: "cambia en menos de ½ beat"** no dice desde cuándo se mide. Implementado: es
    corte si el deck en el aire cambia sin que hubiera una transición `blend` activa durante al
    menos ½ beat.
18. **`deck.gate` `open`.** "Vuelve (sube de 0.05 a más de 0.15)" se implementa solo después de
    un `closed` por corte rápido; si no, cada entrada normal de un deck generaría un `open`.
19. **Histéresis del deck en el aire.** No especificada. Implementado: cambia cuando otro deck
    supera al actual en más de 0,02 de `audible`.

## Límites (§15.7)

20. **Conexiones sin autenticar.** La implementación en Python cuenta desde que termina la
    negociación WebSocket (el `open_timeout` de 2 s protege la fase anterior). La de C++ cuenta
    desde la conexión TCP.

## OS2L (§16.1)

21. El plugin OS2L de QLC+ escucha en el puerto TCP **9996** y separa los mensajes buscando
    `}`: varios mensajes juntos en una lectura funcionan, pero un `}` dentro de un nombre de botón
    cortaría el mensaje. La pasarela usa nombres sin llaves, `TCP_NODELAY`, y envía los `beat`
    sin esperar a la cola de botones para no retrasarlos.
22. Además del mínimo de §16.1, la pasarela envía (desactivable con `--no-extras`):
    `vertexav.countdown.<n>`, `vertexav.transition`, `vertexav.mix.cut`,
    `vertexav.intensity.<hint>`, `vertexav.mood.<mood>`, `vertexav.strobe_ok` y la energía como
    `cmd` 1 (0–100).

## Integración en Mixxx (§17.1)

23. **Anclar el tempo al sample que suena, no al que se procesa.** La posición que el motor
    publica en los controles corresponde al búfer recién calculado, que tarda un búfer o más en
    llegar a la tarjeta. Mixxx guarda en `VisualPlayPosition` el instante en que cada búfer
    llega al DAC; el emisor ancla con la posición interpolada a "ahora". Propuesta para §17.1:
    "el emisor **DEBERÍA** anclar el tempo a la posición que está sonando en la salida, no a la
    procesada".
24. **Retraso después de la tarjeta.** Procesadores, retardos del sistema de sonido y distancia
    añaden un retraso que el emisor no puede medir. El emisor de Mixxx tiene un "desfase de
    salida" configurable que suma a los instantes publicados. Propuesta: mencionarlo en §7.3 como
    contrapartida en el emisor de `latency_us` del receptor.
25. **Secciones sin análisis.** Mixxx no detecta secciones. Las toma de las etiquetas de los
    hotcues (`Drop`, `Build`, `Breakdown`, `Intro`, `Outro`, `Chorus`/`Estribillo`,
    `Verse`/`Verso`, `Bridge`/`Puente`) y de los marcadores de intro y outro, que §11.2 ya pone
    como fuente prioritaria. Propuesta: publicar en la especificación ese vocabulario de
    etiquetas para que todos los programas lo entiendan igual.
26. **Fase en los saltos.** El emisor de Mixxx mantiene un contador de beats maestro con un
    desplazamiento entero respecto al deck de origen. Tras un salto o un cambio de deck, si la
    parte fraccionaria coincide (beatjump, loop, mezcla en fase) no hay `phase_reset`; si no
    coincide, lo hay. Esta regla sencilla cubre los casos 5 y 6 de §9.4.
