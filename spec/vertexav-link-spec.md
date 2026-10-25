# VertexAV Link — Especificación

**Protocolo abierto de información musical en tiempo real para DJ, luces y visuales**

| | |
|---|---|
| **Versión** | 0.6 (borrador) |
| **Estado** | Propuesta para discusión |
| **Fecha** | 2026-10-25 |
| **Autor** | VertexAV — vertexav.mx |

---

## Cambios en 0.6

- Nuevo nombre: **VertexAV Link** (antes VertexAV Protocol). Los identificadores técnicos `vertexav` no cambian.
- Sin resincronización hacia atrás: dentro de una sesión `seq` nunca retrocede, así que reenviar paquetes antiguos firmados no tiene efecto (§8.3).
- Límites de tamaño de mensaje durante toda la sesión, en ambos sentidos, y de anidamiento JSON (§15.7).
- El reloj de show empieza en 0 al arrancar el emisor; todos los instantes en JSON caben sin pérdida en un número de doble precisión (§7.1). Los puentes convierten los instantes a su propio reloj (§5.5).
- El búfer de jitter se vacía al cambiar de sesión o al reconectar, e ignora duplicados (§12.5.1).

## Cambios en 0.5

- Memoria compartida a prueba de emisores caídos: espera del seqlock limitada, máximo de reintentos, testigo de vida `heartbeat_t`, `instance_id` y reglas para soltar el búfer (§12.4.1).
- Límites de tiempo y de conexiones durante la conexión y la autenticación, contra conexiones lentas o masivas (§15.7).
- `path` en el anuncio DNS-SD para descartar bucles antes de conectar; los puentes retiran su anuncio al perder la fuente; reconexión con espera aleatoria (§5.5).
- Validación de longitud de los paquetes UDP: firma en los últimos 8 bytes y longitud exacta por tipo de paquete (§8.3).

## Cambios en 0.4

- Seguridad rediseñada (§15): modos `open`, `auth` y `tls`; emparejamiento con clave de 256 bits; desafío-respuesta mutuo (la clave nunca viaja); firma HMAC en todos los paquetes UDP.
- Números de secuencia robustos: aritmética de número de serie, ventanas de aceptación y resincronización, para que un paquete falso o corrupto no bloquee el flujo (§8.3).
- Memoria compartida sin lecturas corruptas: seqlock en la cabecera y validación después de copiar (§12.4).
- Cortes rápidos: `transition` se activa sin esperar, nuevo `transition.kind` (`blend`/`cut`) y eventos `mix.cut` y `deck.gate` (§10.2).
- Puentes y topología: identificador por emisor, `origin`, `hops`, `path` y detección de bucles (§5.5).

## Cambios en 0.3

- Nombre: **VertexAV Protocol**. Identificadores técnicos con `vertexav` (servicio DNS-SD `_vertexav._tcp`, ruta `/vertexav`, cabeceras binarias `VXAV` y `VXAA`).
- Versión `0.3` en todos los ejemplos y en el anuncio de descubrimiento.
- Todos los ejemplos JSON son válidos; las listas abreviadas se indican en el texto.
- La sesión se identifica en binario con `session_key` (uint32 enviado en `welcome`), sin hash (§6.3, §8.3).
- `rtt_min` definido como el mínimo de las últimas 32 muestras (§7.2).
- Ejemplos de `subscribe` coherentes con los perfiles, con tabla de temas por perfil (§6.4).
- Zona muerta: los extremos (0, 1) y los cruces por 0 se envían al instante; valor final a los 50 ms (§8.4).

## Cambios en 0.2

- Nombre provisional: **Vertex Protocol**.
- La sincronización de reloj pasa al canal UDP, con filtrado de muestras y corrección gradual (§7.2).
- Límites de frecuencia y zona muerta para los valores continuos del canal de control (§8.4).
- Comportamiento definido para scratch, pitch bend, Slip Mode y saltos, con `stability` y `phase_reset` (§9.4).
- Ableton Link: VertexAV refleja la línea de tiempo de Link en vez de imponerse (§16.4).
- Unicast por defecto; multicast solo en redes cableadas (§5.3).
- Búfer de jitter adaptativo y ocultación de pérdidas para el audio remoto (§12.5.1).

---

## Índice

1. [Introducción](#1-introducción)
2. [Terminología](#2-terminología)
3. [Arquitectura](#3-arquitectura)
4. [Perfiles](#4-perfiles)
5. [Transporte y descubrimiento](#5-transporte-y-descubrimiento)
6. [Sesión: conexión, suscripción y latido](#6-sesión-conexión-suscripción-y-latido)
7. [Reloj y sincronía](#7-reloj-y-sincronía)
8. [Formato de mensajes](#8-formato-de-mensajes)
9. [Perfil 0 — Beat](#9-perfil-0--beat)
10. [Perfil 1 — Estado](#10-perfil-1--estado)
11. [Perfil 2 — Estructura](#11-perfil-2--estructura)
12. [Perfil 3 — Audio](#12-perfil-3--audio)
13. [Mensajes de retorno (receptor → emisor)](#13-mensajes-de-retorno-receptor--emisor)
14. [Errores](#14-errores)
15. [Seguridad](#15-seguridad)
16. [Pasarelas a otros protocolos](#16-pasarelas-a-otros-protocolos)
17. [Guía de implementación](#17-guía-de-implementación)
18. [Conformidad](#18-conformidad)
19. [Cuestiones abiertas](#19-cuestiones-abiertas)
20. [Anexo A — Ejemplo de sesión completa](#anexo-a--ejemplo-de-sesión-completa)
21. [Anexo B — Enumeraciones](#anexo-b--enumeraciones)

---

## 1. Introducción

### 1.1 Objetivo

VertexAV es un protocolo abierto para que el software de DJ (por ejemplo **Mixxx**) comparta en tiempo real **todo lo que sabe de la música que suena** con software de iluminación (por ejemplo **QLC+**), visualizadores (por ejemplo **projectM**) y cualquier otro receptor.

### 1.2 Principio de diseño

> **VertexAV describe la música; no da órdenes.**

OS2L dice "pulsa el botón 5". VertexAV dice "viene un drop dentro de 8 beats, energía alta, tema en La menor". Cada receptor decide qué hacer con esa información: QLC+ elige escenas, projectM elige presets.

### 1.3 Qué resuelve frente a lo existente

| | Beat/BPM | Qué tema | Posición | Estructura | Mezcla | Audio | Abierto |
|---|---|---|---|---|---|---|---|
| OS2L | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ |
| MIDI | ⚠️ | ❌ | ❌ | ❌ | ⚠️ | ❌ | ✅ |
| Ableton Link | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ |
| Pro DJ Link | ✅ | ✅ | ✅ | ⚠️ | ✅ | ❌ | ❌ |
| StageLinQ | ✅ | ✅ | ✅ | ❌ | ✅ | ❌ | ❌ |
| **VertexAV** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

### 1.4 Objetivos

- Sincronía precisa: los eventos llevan **hora exacta** y se anuncian **por adelantado**.
- Adopción gradual: **perfiles** para que un receptor sencillo implemente solo lo básico.
- Independencia de marca: cualquier software de DJ, de luces o de visuales.
- Funciona en el mismo PC o entre varios equipos de una red local.
- Reutilizar estándares existentes cuando ya resuelven el problema (Ableton Link, DNS-SD, WebSocket).

### 1.5 Fuera de alcance

- Control directo de aparatos DMX (eso lo hace el software de luces).
- Transmisión de audio por Internet o con compresión con pérdidas.
- Sincronización de vídeo fotograma a fotograma.

---

## 2. Terminología

Las palabras **DEBE**, **NO DEBE**, **DEBERÍA**, **NO DEBERÍA** y **PUEDE** tienen el significado de MUST, MUST NOT, SHOULD, SHOULD NOT y MAY del RFC 2119.

| Término | Significado |
|---|---|
| **Emisor** | Programa que conoce la música y publica datos VertexAV (Mixxx, un puente). |
| **Receptor** | Programa que consume datos VertexAV (QLC+, projectM). |
| **Puente** | Programa que actúa como receptor de una fuente y emisor hacia otros; por ejemplo, añade análisis de estructura. |
| **Deck** | Reproductor virtual del software de DJ. Se identifica con un entero desde 1. |
| **Deck en el aire** | El deck que más se oye en la mezcla en ese momento. |
| **Reloj de show** | Reloj monotónico del emisor, en microsegundos, al que se refieren todos los tiempos. |
| **Canal de control** | Conexión fiable (WebSocket) para estado, mapas de temas y eventos. |
| **Canal de tiempo real** | Flujo UDP para datos de alta frecuencia (beat, niveles, espectro). |
| **Snapshot** | Estado completo actual que el emisor envía al conectarse un receptor. |

---

## 3. Arquitectura

```
                         ┌──────────────────────────────┐
                         │           EMISOR             │
                         │  (Mixxx o puente VertexAV)      │
                         └──────┬───────────┬───────────┘
              Canal de control  │           │  Canal de tiempo real
              WebSocket (JSON)  │           │  UDP (binario)
                                │           │
         ┌──────────────────────┼───────────┼───────────────────────┐
         │                      │           │                       │
   ┌─────▼─────┐          ┌─────▼───────────▼──┐          ┌─────────▼────────┐
   │   QLC+    │          │     projectM       │          │  Otro receptor   │
   │ Perfil 0-2│          │   Perfil 0-3       │          │  (vídeo, láser)  │
   └───────────┘          └────────▲───────────┘          └──────────────────┘
                                   │
                          Memoria compartida
                          (audio PCM, mismo PC)
```

- Un emisor atiende a **varios receptores** a la vez.
- Un receptor **PUEDE** conectarse a varios emisores (por ejemplo, dos PCs de DJ en un relevo), pero en cada momento **DEBERÍA** seguir a uno solo.
- Un puente se presenta como receptor ante su fuente y como emisor ante los demás. Las reglas para evitar bucles están en §5.5.

---

## 4. Perfiles

Cada perfil incluye los anteriores.

| Perfil | Nombre | Contenido | Caso de uso típico |
|---|---|---|---|
| **0** | Beat | Tempo, fase, compás, próximos beats | Luces al ritmo (sustituto preciso de OS2L) |
| **1** | Estado | + decks, mezcla, metadatos, eventos, intención, disparos manuales | Luces que siguen la mezcla |
| **2** | Estructura | + mapa del tema (secciones, energía, marcadores), cuentas atrás | Nivel "SoundSwitch": shows por tema |
| **3** | Audio | + características de audio y audio PCM opcional | Visualizadores |

El emisor anuncia los perfiles que soporta. El receptor indica cuáles quiere. Se usa la intersección.

---

## 5. Transporte y descubrimiento

### 5.1 Descubrimiento (DNS-SD / mDNS)

El emisor **DEBE** anunciarse con DNS-SD:

- **Servicio:** `_vertexav._tcp`
- **Puerto:** el del canal de control.
- **Registros TXT:**

| Clave | Ejemplo | Significado |
|---|---|---|
| `v` | `0.6` | Versión del protocolo |
| `role` | `emitter` | `emitter` o `bridge` |
| `id` | `vx-3f9c2a` | Identificador único y estable de este emisor o puente (§5.5) |
| `origin` | `vx-3f9c2a` | `id` de la fuente primaria (el software de DJ). En un emisor primario, igual a `id` |
| `hops` | `0` | Número de puentes entre la fuente primaria y este anuncio (0 = fuente primaria) |
| `path` | `vx-3f9c2a` | Lista de `id` desde la fuente primaria hasta este anuncio, separados por comas (por ejemplo, `vx-3f9c2a,vx-b71e04`). Cabe de sobra en el límite de 255 bytes de un registro TXT con el máximo de 4 saltos |
| `name` | `Mixxx (Cabina 1)` | Nombre legible |
| `profiles` | `0,1,2,3` | Perfiles soportados |
| `security` | `auth` | Modo de seguridad: `open`, `auth` o `tls` (§15.2) |
| `scope` | `lan` | `lan` o `local` (solo escucha en este equipo) |

Los receptores **DEBERÍAN** permitir además conectar indicando IP y puerto a mano.

### 5.2 Canal de control

- **WebSocket** sobre TCP. Puerto por defecto **9470** (provisional).
- Mensajes de texto **JSON en UTF-8**, un mensaje por trama.
- Ruta: `ws://<host>:9470/vertexav`.
- Se usa para: sesión, snapshot, estado de decks, mapas de temas, eventos, intención, disparos y retorno.

### 5.3 Canal de tiempo real

- **UDP**. Puerto del emisor por defecto **9471** (provisional).
- El emisor envía por **unicast** a cada receptor suscrito, a la dirección y puerto que este indicó en `hello`.
- **Unicast es el modo por defecto y el recomendado.** Es el único que **DEBE** usarse en redes Wi-Fi: allí el multicast se transmite a la velocidad básica más baja y sin retransmisiones, con pérdidas altas.
- El emisor **PUEDE** ofrecer **multicast** en `239.255.94.70:9471` solo en redes **cableadas** con muchos receptores, y **NO DEBE** activarlo por defecto.
- Despliegue recomendado: emisor y receptores críticos (software de luces, nodos Art-Net) por cable; Wi-Fi solo para receptores no críticos.
- Paquetes **binarios** (§8.3). Se usa para: sincronización de reloj, beat, niveles, espectro y audio PCM remoto.
- Un receptor que no pueda usar UDP **PUEDE** pedir los mismos datos por el canal de control (`realtime_over_control: true`), aceptando más carga.

### 5.4 Memoria compartida (solo perfil 3, mismo equipo)

Para el audio PCM local se usa un búfer circular en memoria compartida (§12.4). Es el método más eficiente: sin copias por red ni serialización.

### 5.5 Puentes y topología

Un puente (por ejemplo, uno que añade análisis de estructura) recibe de un emisor y vuelve a emitir. Sin reglas, los receptores podrían conectarse en círculo entre puentes. Para evitarlo:

**Identidad**
- Todo emisor y puente **DEBE** tener un `id` único y estable (por ejemplo, `vx-` + 6 a 16 caracteres aleatorios generados en la instalación).
- El nombre de instancia DNS-SD **DEBE** ser propio. Un puente **NO DEBE** copiar el nombre de su fuente; se recomienda el formato "`<nombre del puente>` (vía `<nombre de la fuente>`)".

**Jerarquía**
- Un emisor primario anuncia `origin` = su propio `id` y `hops` = 0.
- Un puente anuncia su `id`, el `origin` de su fuente y `hops` = `hops` de la fuente + 1.
- En `welcome`, el campo `emitter.path` lista los `id` desde la fuente primaria hasta este emisor (por ejemplo, `["vx-3f9c2a", "vx-b71e04"]`).

**Prevención de bucles**
- **Antes de conectar:** un puente **DEBE** leer el `path` del anuncio DNS-SD y **NO DEBE** abrir conexión con un emisor cuyo `path` contenga su propio `id`. Así se descartan los bucles sin abrir sockets ni hacer cálculos criptográficos.
- **Después de conectar:** el anuncio DNS-SD no está autenticado, así que lo que vale es el `path` de `welcome`. Si contiene el `id` del puente, este cierra con `error` `loop_detected` y no vuelve a intentar con ese emisor durante **60 s**.
- **Retirada al perder la fuente:** si un puente pierde su fuente, **DEBE** retirar su anuncio DNS-SD (anuncio de despedida con TTL 0) en menos de **1 s**, para que ningún otro puente lo elija como fuente. Mantiene a sus receptores conectados, les envía el evento `source.lost` y se vuelve a anunciar al recuperar la fuente (evento `source.restored`).
- **Reconexión con espera aleatoria:** los puentes reconectan con espera exponencial y variación aleatoria: 0,5 s, 1 s, 2 s… hasta **30 s**, cada valor ± 25 %. Así varios puentes no reconectan a la vez en ciclos sincronizados.
- Un puente **DEBE** ignorar sus propios anuncios.
- `hops` máximo: **4**. Los anuncios con `hops` mayor se ignoran.

**Elección automática de fuente (receptores)**
1. Si el usuario eligió un emisor concreto, se usa ese.
2. Si no, entre los anuncios con el mismo `origin`, el receptor elige el que ofrezca todos los perfiles que necesita con el menor `hops`.
3. Ante empate, el de menor `rtt`.

**Reloj**
- Un puente tiene su propio reloj de show y su propia sesión con sus receptores. **DEBE** convertir todos los instantes de su fuente (`t`, `at`, `anchor_t`, `target_at`…) a su propio reloj antes de reenviarlos.

**Interfaces de red**
- El emisor o puente **DEBE** anunciarse solo en las interfaces donde escucha, con las direcciones de esa interfaz.
- Un puente que solo sirve a programas del mismo equipo (por ejemplo, QLC+ en el mismo PC) **DEBERÍA** escuchar solo en `127.0.0.1`, anunciar `scope=local` y no anunciarse en la red local.

---

## 6. Sesión: conexión, suscripción y latido

### 6.1 Secuencia

```
Receptor                                   Emisor
   │──── WebSocket connect ───────────────────►│
   │──── hello ───────────────────────────────►│
   │◄─────────────────────────────── welcome ──│
   │──── clock.ping UDP (x N) ────────────────►│
   │◄──────────────────────── clock.pong UDP ──│
   │──── subscribe ───────────────────────────►│
   │◄────────────────────────────── snapshot ──│
   │◄═══════ mensajes de control (deltas) ═════│
   │◄─ ─ ─ ─ paquetes UDP de tiempo real ─ ─ ─ │
   │──── heartbeat (cada 1 s) ────────────────►│
   │◄────────────────────────────── heartbeat ─│
```

Con los modos de seguridad `auth` o `tls`, entre `hello` y `welcome` hay un intercambio `auth.challenge` / `auth.response` (§15.4). Ningún mensaje lleva la clave compartida.

### 6.2 `hello` (receptor → emisor)

```json
{
  "type": "hello",
  "protocol": "vertexav",
  "version": "0.6",
  "role": "receiver",
  "name": "QLC+ 4.14",
  "client_id": "c1f2-qlcplus-01",
  "profiles": [0, 1, 2],
  "latency_us": 25000,
  "udp_port": 52110,
  "realtime_over_control": false
}
```

| Campo | Tipo | Obligatorio | Descripción |
|---|---|---|---|
| `protocol` | string | Sí | Siempre `"vertexav"` |
| `version` | string | Sí | Versión que habla el receptor |
| `role` | string | Sí | `receiver` o `bridge` |
| `name` | string | Sí | Nombre legible |
| `client_id` | string | Sí | Identificador estable del receptor, para reconexiones |
| `profiles` | int[] | Sí | Perfiles deseados |
| `latency_us` | int | No | Retraso propio del receptor hasta el efecto visible (interfaz DMX, GPU…). Por defecto 0 |
| `udp_port` | int | No | Puerto donde recibir el tiempo real. Si falta, no se envía UDP |
| `realtime_over_control` | bool | No | Recibir el tiempo real por WebSocket |

### 6.3 `welcome` (emisor → receptor)

```json
{
  "type": "welcome",
  "version": "0.6",
  "session_id": "8d3a91",
  "session_key": 2309471281,
  "emitter": { "id": "vx-3f9c2a", "name": "Mixxx 2.6", "role": "emitter", "path": ["vx-3f9c2a"] },
  "security": "auth",
  "server_proof": "gDCn2ix3Rq8nn/tM7VHOzjdofFOF0XYaIGSOVbZ3Gug=",
  "profiles": [0, 1, 2],
  "udp_port": 9471,
  "multicast": null,
  "decks": 4,
  "lookahead_us": 150000,
  "link": { "enabled": true }
}
```

| Campo | Descripción |
|---|---|
| `profiles` | Perfiles acordados (intersección) |
| `session_id` | Identificador legible de la sesión |
| `session_key` | Entero de 32 bits sin signo que identifica la sesión en todos los paquetes binarios (§8.3). El emisor lo genera al azar al arrancar y **NO DEBE** ser 0. **No es un secreto**: solo sirve para separar sesiones |
| `emitter` | `id`, nombre, rol y `path` desde la fuente primaria (§5.5) |
| `security` | Modo de seguridad de la sesión (§15.2) |
| `server_proof` | Prueba de que el emisor conoce la clave compartida (§15.4). `null` en modo `open` |
| `decks` | Número de decks del emisor |
| `lookahead_us` | Con cuánta antelación el emisor publica eventos programados (§7.4) |
| `link` | Si el emisor participa en una sesión de Ableton Link (§16.4) |

### 6.4 `subscribe`

El receptor elige qué temas quiere. Si no envía `subscribe`, recibe todo lo de sus perfiles.

```json
{
  "type": "subscribe",
  "topics": ["tempo", "deck", "mix", "track", "structure", "event", "intent", "trigger"]
}
```

Ejemplo de un visualizador con perfiles 0–3:

```json
{
  "type": "subscribe",
  "topics": ["tempo", "track", "structure", "event", "intent", "features", "audio"],
  "features": { "sources": ["master", "deck1", "deck2"], "bands": 16, "rate_hz": 60 }
}
```

Temas disponibles:

| Tema | Perfil mínimo | Contenido |
|---|---|---|
| `tempo` | 0 | Tempo y fase (§9) |
| `deck` | 1 | `deck.state` |
| `mix` | 1 | `mix.state` |
| `track` | 1 | `track.loaded`, `track.unloaded` |
| `event` | 1 | `event`, `event.cancel` |
| `intent` | 1 | `intent` |
| `trigger` | 1 | `trigger` |
| `structure` | 2 | `track.structure`, `section.change`, `countdown` |
| `features` | 3 | Paquetes `features` por UDP (§12.3) |
| `audio` | 3 | Permite pedir audio PCM; el flujo se abre con `audio.subscribe` (§12.4) |

Un tema que exige un perfil no acordado en `welcome` se ignora, y el emisor responde `error` `unsupported_profile` (no fatal).

### 6.5 Latido y reconexión

- Ambos lados **DEBEN** enviar `{"type":"heartbeat","t":50100000}` (`t` = reloj de show del emisor, o reloj local si lo envía el receptor) cada **1 s** si no han enviado otra cosa.
- Si no llega nada en **3 s**, la conexión se considera caída.
- Al reconectar con el mismo `client_id`, el emisor **DEBE** enviar un `snapshot` nuevo.
- El receptor **DEBERÍA** reintentar con espera creciente (0,5 s, 1 s, 2 s… hasta 5 s), cada valor ± 25 % aleatorio.
- Mientras está desconectado, el receptor **DEBERÍA** seguir extrapolando el tempo con los últimos datos durante al menos 8 beats, para que las luces no se paren de golpe.

### 6.6 Cierre

`{"type":"bye","reason":"shutdown"}` antes de cerrar el WebSocket.

---

## 7. Reloj y sincronía

### 7.1 Reloj de show

- Todos los tiempos (`t`, `at`, etc.) se expresan en **microsegundos del reloj de show del emisor** (entero de 64 bits, monotónico).
- El reloj de show **DEBE** empezar en **0** (o cerca de 0) cada vez que arranca el emisor, y no se basa en la fecha del sistema. Así los valores son pequeños: el límite de 2⁵³ que los números JSON (doble precisión) representan sin pérdida tardaría unos 285 años en alcanzarse.
- Todos los instantes y enteros grandes enviados como número en JSON **DEBEN** ser menores que 2⁵³. Los identificadores de 64 bits que puedan superarlo (como `instance_id`) van como texto hexadecimal.
- Los receptores que usen su propio reloj en mensajes JSON (por ejemplo, `t0` en `clock.ping` por el canal de control) **DEBERÍAN** usar también un reloj monotónico que empiece cerca de 0.
- Las posiciones dentro de un tema se expresan en **segundos (float64)** desde el inicio del archivo.
- Las posiciones musicales se expresan en **beats (float64)**.

### 7.2 Sincronización del reloj

El receptor estima el desfase con su reloj local mediante un intercambio tipo NTP **por el canal UDP de tiempo real** (paquetes `kind` 0x02 y 0x03, §8.3). No se usa el canal de control: TCP añade retransmisiones, bloqueo de cabeza de línea y jitter que falsean la medida.

**`clock.ping` (receptor → emisor, UDP, `kind` 0x02)**, tras la cabecera común (en el ping, el campo `t` de la cabecera va a 0):

| Offset | Tamaño | Campo |
|---|---|---|
| 24 | 8 | `t0` (int64): reloj local del receptor al enviar |
| 32 | 4 | `ping_id` (uint32) |

**`clock.pong` (emisor → receptor, UDP, `kind` 0x03)**:

| Offset | Tamaño | Campo |
|---|---|---|
| 24 | 8 | `t0` (copiado del ping) |
| 32 | 4 | `ping_id` (copiado del ping) |
| 36 | 8 | `t1` (int64): reloj del emisor al recibir |
| 44 | 8 | `t2` (int64): reloj del emisor al responder |

El emisor **DEBE** tomar `t1` lo antes posible al recibir, `t2` justo antes de enviar, y responder con máxima prioridad.

Solo si el receptor no puede usar UDP (`realtime_over_control`), **PUEDE** hacer el mismo intercambio por el canal de control (`{"type":"clock.ping","t0":1234567890,"ping_id":7}` / `{"type":"clock.pong","t0":1234567890,"ping_id":7,"t1":9876543210,"t2":9876543260}`), sabiendo que la precisión será peor.

- `t0`: reloj local del receptor al enviar.
- `t1`: reloj del emisor al recibir.
- `t2`: reloj del emisor al responder.
- Con `t3` (reloj local al recibir la respuesta):
  - `rtt = (t3 - t0) - (t2 - t1)`
  - `offset = ((t1 - t0) + (t2 - t3)) / 2`

Filtrado recomendado:

- Al conectar, al menos **16 intercambios** espaciados 20–50 ms.
- Mantener una **ventana deslizante con las últimas 32 muestras** (al conectar, las 16 iniciales). `rtt_min` es el `rtt` mínimo dentro de esa ventana. Se usa una ventana por número de muestras, no por tiempo, para que todos los receptores se comporten igual.
- Descartar las muestras de la ventana con `rtt` mayor que 1,5 × `rtt_min` (las que sufrieron colas o reintentos de Wi-Fi).
- Usar la **mediana** del `offset` de las muestras restantes de la ventana.
- Repetir un intercambio cada **1–2 s** y corregir la deriva **de forma gradual** (sin saltos de más de 1 ms por segundo), para que los efectos no den tirones.
- Si `rtt_min` supera **20 ms** durante más de **10 s**, el receptor **DEBERÍA** avisar al usuario de que la red no es apta (típico de Wi-Fi saturado).

### 7.3 Compensación de latencia

Cada receptor conoce su propio retraso (`latency_us`). Para que un efecto se vea justo en el instante `at`, el receptor lo **DEBE** ejecutar en `at - latency_us` (convertido a su reloj local).

### 7.4 Publicación anticipada (lookahead)

- El emisor **DEBERÍA** publicar los eventos previsibles (beats, cambios de sección, drops) **antes de que ocurran**, con `lookahead_us` de antelación (recomendado: 100–200 ms).
- Si el DJ cambia algo (salto de cue, pitch, pausa) y un evento anunciado ya no ocurrirá, el emisor **DEBE** enviar `event.cancel` con su `event_id` (§10.6).
- Los eventos no previsibles (disparos manuales, scratch) se envían en cuanto ocurren, con `at` igual al instante actual.

---

## 8. Formato de mensajes

### 8.1 Mensajes de control (JSON)

Todos los mensajes JSON comparten esta envoltura:

| Campo | Tipo | Obligatorio | Descripción |
|---|---|---|---|
| `type` | string | Sí | Tipo de mensaje, en minúsculas con puntos (`deck.state`) |
| `t` | int64 | Recomendado | Reloj de show del emisor al generar el mensaje |
| `seq` | int | No | Número de secuencia creciente por conexión |

Reglas:

- Los receptores **DEBEN** ignorar los campos desconocidos.
- Los receptores **DEBEN** ignorar los tipos de mensaje desconocidos.
- Los campos opcionales ausentes significan "sin cambios" en los mensajes delta y "desconocido" en los completos.
- Todos los ejemplos de esta especificación son JSON válido. Cuando una lista está abreviada, se indica en el texto.
- Los mensajes de prefijo `x-` (`x-mixxx.waveform`) están reservados para extensiones propias.

### 8.2 Estado completo y deltas

- El `snapshot` contiene el estado completo de todos los temas suscritos.
- Después, el emisor envía **deltas**: mensajes con solo los campos que cambiaron.
- Cada mensaje de estado lleva su `t`. Un receptor **DEBE** descartar un delta más antiguo que el último aplicado para el mismo objeto.

### 8.3 Paquetes de tiempo real (UDP, binario)

Todos los enteros en **little-endian**.

**Cabecera común (24 bytes):**

| Offset | Tamaño | Campo | Descripción |
|---|---|---|---|
| 0 | 4 | `magic` | `VXAV` (0x56 0x58 0x41 0x56) |
| 4 | 1 | `version` | Versión mayor del formato binario (0 en este borrador) |
| 5 | 1 | `kind` | Tipo de paquete (tabla siguiente) |
| 6 | 2 | `flags` | Bit 0 = el paquete lleva firma al final (§15.5). Resto reservado, 0 |
| 8 | 4 | `seq` | Número de secuencia por tipo |
| 12 | 4 | `session` | `session_key` (uint32) recibido en `welcome` (§6.3). También en los `clock.ping` del receptor |
| 16 | 8 | `t` | Reloj de show (µs) al que se refiere el contenido |

**Tipos de paquete:**

| `kind` | Nombre | Perfil | Frecuencia típica |
|---|---|---|---|
| 0x01 | `tempo` | 0 | 20 Hz + en cada beat |
| 0x02 | `clock.ping` (receptor → emisor) | 0 | Al conectar y cada 1–2 s |
| 0x03 | `clock.pong` (emisor → receptor) | 0 | En respuesta a cada ping |
| 0x10 | `features` | 3 | 30–120 Hz |
| 0x20 | `audio` | 3 | Según bloque de audio |

El tamaño total de un paquete, incluida la firma, **NO DEBERÍA** superar 1200 bytes, para evitar fragmentación.

**Orden de validación** de cada paquete recibido:

1. **Longitud mínima:** el receptor **DEBE** leer con un búfer de al menos 1500 bytes y descartar los datagramas truncados (por ejemplo, los marcados con `MSG_TRUNC`). Un datagrama de menos de 24 bytes (cabecera), o de menos de 32 si la sesión exige firma, se descarta.
2. `magic` y `version` correctos.
3. `session` igual al `session_key` de la sesión.
4. **Firma** (modos `auth` y `tls`): el bit 0 de `flags` **DEBE** valer 1. La firma son los **últimos 8 bytes** del datagrama (desde `longitud - 8`) y se calcula sobre los bytes `[0, longitud - 8)` (§15.5). Si falta o no es correcta, se descarta **sin mirar `seq`**.
5. **Longitud exacta:** sin contar la firma, la longitud **DEBE** coincidir con la que corresponde al tipo de paquete. Si no, se descarta:

   | `kind` | Longitud sin firma |
   |---|---|
   | 0x01 `tempo` | 68 bytes |
   | 0x02 `clock.ping` | 36 bytes |
   | 0x03 `clock.pong` | 52 bytes |
   | 0x10 `features` | 40 + 4 × `band_count` |
   | 0x20 `audio` | 40 + `frames` × `channels` × bytes por muestra (2 en `s16le`, 4 en `f32le`) |

6. `t` razonable: se descarta si difiere más de **2 s** de la estimación actual del reloj del emisor (protege de paquetes reenviados más tarde).
7. Reglas de `seq` (abajo).

**Números de secuencia.** `seq` se compara con aritmética de número de serie (RFC 1982), así que el paso de 4 294 967 295 a 0 es normal. Para cada `kind` (y cada `source_id` en `features`), el receptor guarda el último `seq` aceptado, `last`:

| Caso | Acción |
|---|---|
| `seq` posterior a `last` en 1–256 | Se acepta y pasa a ser `last` |
| `seq` anterior o igual a `last` en 0–64 | Duplicado o desordenado: se descarta |
| `seq` posterior a `last` en más de 256 (salto grande adelante) | No se acepta todavía. Si llegan **8 paquetes seguidos con `seq` consecutivos** desde ese nuevo punto, el receptor adopta la nueva secuencia |
| `seq` anterior a `last` en más de 64 (salto grande atrás) | Se descarta **siempre**. Dentro de una sesión nunca se resincroniza hacia atrás |

Así, un paquete corrupto o falso con un `seq` muy alto no bloquea el flujo real: como mucho se pierden unos pocos paquetes, que el receptor cubre extrapolando el tempo. En modo `open` un atacante que inyecte paquetes de forma continua aún puede molestar; por eso ese modo solo se recomienda en redes cerradas (§15.2).

- Dentro de una sesión, el emisor **NO DEBE** reiniciar ni hacer retroceder `seq`. Si el emisor se reinicia, siempre crea una sesión nueva (nuevo `session_key`), y con ella el receptor borra todo su estado de secuencias y acepta el primer paquete que llegue.
- Por eso **reenviar paquetes antiguos** (replay) no tiene efecto: aunque su firma sea válida, su `seq` está atrasado y se descartan. Y no se pueden fabricar paquetes con `seq` futuro sin la clave.

**Excepción:** los paquetes de audio (`kind` 0x20) no se descartan por orden; se reordenan en el búfer de jitter (§12.5.1). Sí se aplican los pasos 1–6. Allí se descartan los duplicados y los paquetes cuyo instante ya pasó, así que reenviar audio antiguo tampoco tiene efecto.

### 8.4 Límites de frecuencia en el canal de control

Los controles continuos (faders, crossfader, EQ, filtro, `audible`, `transition.progress`, `energy`) pueden cambiar cientos de veces por segundo al mover un fader o barrer un filtro. Para no saturar el canal de control:

- **Agrupación:** el emisor **DEBE** acumular los cambios de valores continuos y enviarlos como máximo a **20 Hz por objeto** (por deck, por mezcla, por intención). Si un valor cambia varias veces entre envíos, solo se manda el último.
- **Zona muerta:** el emisor **NO DEBERÍA** enviar un valor continuo que cambió menos de **0,01** (1 % del rango) respecto al último enviado.
- **Extremos y cruces por cero sin retraso:** cuando un valor continuo **llega a un extremo de su rango** (0 o 1; -1, 0 o 1 en crossfader y filtro) o **pasa de 0 a otro valor o al revés**, el emisor **DEBE** enviarlo **de inmediato**, saltándose la zona muerta y el límite de 20 Hz. Así, un fader cerrado o un deck que deja de oírse llega al instante como 0 exacto, sin "fugas" en luces ni visuales.
- **Valor final:** cuando el control se detiene (**50 ms** sin cambios), el emisor envía el valor exacto si difiere del último enviado.
- **Valores discretos** (play, loop, kills de EQ, carga de tema, eventos, disparos) **DEBEN** enviarse de inmediato, sin agrupar ni zona muerta.
- **Un solo delta por objeto:** si cambian varios campos del mismo objeto en el mismo intervalo, van en un único mensaje.
- Objetivo: menos de **100 mensajes por segundo** por conexión en uso normal.
- Si un receptor necesita valores continuos más fluidos (por ejemplo, un dimmer que siga al fader), **DEBERÍA** interpolar entre los valores recibidos, o usar el canal UDP (`features`), que ya va a 60 Hz.
- El emisor **PUEDE** responder `error` `rate_limited` a un receptor que envíe más de 50 mensajes por segundo.

---

## 9. Perfil 0 — Beat

### 9.1 Modelo

El tempo se describe con un **ancla** y una **pendiente**: en el instante `anchor_t` la música está en el beat `anchor_beat`, y avanza `bpm/60` beats por segundo. Con esto cualquier receptor calcula el beat exacto en cualquier instante, sin esperar al siguiente mensaje.

```
beat(t) = anchor_beat + (t - anchor_t) / 1e6 * bpm / 60
```

### 9.2 `tempo` (control y UDP)

**JSON (canal de control, en cada cambio):**

```json
{
  "type": "tempo",
  "t": 50100000,
  "bpm": 126.0,
  "anchor_t": 50100000,
  "anchor_beat": 1024.0,
  "beats_per_bar": 4,
  "bar_offset": 0.0,
  "source_deck": 1,
  "playing": true,
  "confidence": 1.0,
  "stability": "stable",
  "phase_reset": false,
  "link_aligned": null
}
```

| Campo | Tipo | Descripción |
|---|---|---|
| `bpm` | float | Tempo efectivo (incluido el pitch) |
| `anchor_t` | int64 | Instante del ancla |
| `anchor_beat` | float | Beat en el instante del ancla |
| `beats_per_bar` | int | Beats por compás (normalmente 4) |
| `bar_offset` | float | El beat `n` es "1" del compás si `(n - bar_offset) mod beats_per_bar == 0` |
| `source_deck` | int | Deck del que sale el tempo (normalmente el deck en el aire) |
| `playing` | bool | Si hay música sonando con tempo |
| `confidence` | float 0–1 | Fiabilidad del beatgrid |
| `stability` | enum | `stable`, `unstable` o `freewheel` (§9.4) |
| `phase_reset` | bool | El ancla salta a propósito (seek, cambio de deck en el aire, fin de scratch sin slip); el receptor aplica el salto sin suavizar |
| `link_aligned` | bool / null | Solo si el emisor está en Ableton Link: si el tempo publicado coincide con la sesión de Link (§16.4). `null` si no hay Link |

**Binario (UDP, `kind` 0x01), tras la cabecera:**

| Offset | Tamaño | Campo |
|---|---|---|
| 24 | 8 | `bpm` (float64) |
| 32 | 8 | `anchor_t` (int64) |
| 40 | 8 | `anchor_beat` (float64) |
| 48 | 8 | `next_beat_t` (int64): instante del próximo beat |
| 56 | 8 | `next_downbeat_t` (int64): instante del próximo "1" |
| 64 | 1 | `beats_per_bar` (uint8) |
| 65 | 1 | `source_deck` (uint8) |
| 66 | 1 | `flags` (bit 0 = playing, bit 1 = phase_reset, bits 2–3 = stability: 0 stable, 1 unstable, 2 freewheel; bit 4 = link_aligned) |
| 67 | 1 | `confidence` (uint8, 0–255) |

### 9.3 Reglas

- El emisor **DEBE** enviar `tempo` por el canal de control cuando cambie el BPM, el deck de origen, el estado de reproducción o haya un salto de fase.
- El emisor **DEBE** enviar el paquete UDP al menos a 20 Hz mientras haya música, y además con antelación `lookahead_us` antes de cada beat.
- Un receptor de perfil 0 **DEBE** funcionar solo con estos datos.

### 9.4 Manipulaciones en vivo: scratch, pitch bend, Slip Mode y saltos

El tempo de VertexAV representa **la línea de tiempo musical que percibe el público**, no la posición física del plato. Las manipulaciones bruscas no deben convertirse en parpadeos caóticos.

**Estados de estabilidad (`stability`):**

| Estado | Cuándo | Qué hace el receptor |
|---|---|---|
| `stable` | Reproducción normal, pitch fader, nudges pequeños | Extrapola con el ancla y aplica las correcciones de forma suave |
| `unstable` | Scratch, backspin, freno, pitch bend agresivo, `rate` fuera de ±10 % del valor estable o cambiando de signo | **Congela la fase:** sigue con el último tempo estable e ignora las anclas que llegan. **NO DEBE** disparar efectos de beat con anclas inestables |
| `freewheel` | El emisor sigue una línea de tiempo virtual (Slip Mode) | Extrapola con el ancla que envía el emisor, que es la de esa línea virtual |

**Reglas para el emisor:**

1. **Scratch:** `playing` **sigue en `true`** (el tema no se ha parado). `deck.state.scratching` pasa a `true`, `stability` a `unstable` y se envía el evento `scratch.start`. Mientras dura, el emisor **NO DEBE** publicar anclas basadas en la posición física.
2. **Slip Mode:** con slip activo, el emisor **DEBE** publicar el tempo de la **posición de slip** (adonde volverá la música) con `stability: freewheel`. Al soltar, la música vuelve a esa línea y no hay salto de fase.
3. **Fin de la manipulación:** cuando el `rate` vuelve a menos de ±2 % del valor estable durante al menos **1 beat**, el emisor publica un ancla nueva con `stability: stable`. Si la fase cambió (por ejemplo, un scratch sin slip movió el tema), esa ancla lleva `phase_reset: true`.
4. **Pitch bend o nudge suave:** se mantiene `stable` y se envían anclas corregidas; el receptor suaviza la diferencia.
5. **Saltos (hotcue, seek):** ancla nueva con `phase_reset: true` y evento `seek`. Un **beatjump o loop que conserva la fase del compás** **DEBERÍA** llevar `phase_reset: false`, porque el compás no se rompe.
6. **Cambio de deck en el aire** con fase distinta: ancla nueva con `phase_reset: true`.
7. **Pausa o freno completo:** `playing: false` y evento `track.stop`.

**Reglas para el receptor:**

- **Suavizado:** con `phase_reset: false`, el receptor **DEBE** corregir de forma gradual la diferencia entre su fase extrapolada y la nueva ancla. Recomendado: corrección máxima del 2 % del periodo de beat por cada beat, o ajustar el tempo local ±1 % hasta alinear. Si el error supera **¼ de beat**, aplica el salto directamente.
- **Con `phase_reset: true`**, aplica el salto de inmediato, preferiblemente alineado con el siguiente beat para que no se note.
- **Con `unstable`**, sigue el último tempo estable durante un máximo de **8 beats**. Si dura más, deja de generar efectos de beat (o pasa a efectos sin ritmo) hasta recibir `stable`.
- Los efectos que no dependen del beat (intensidad por `energy`, color por `palette`) siguen funcionando con normalidad.

---

## 10. Perfil 1 — Estado

### 10.1 `deck.state`

```json
{
  "type": "deck.state",
  "t": 50100000,
  "deck": 1,
  "track_id": "mixxx:8f2c1e9a",
  "loaded": true,
  "playing": true,
  "position_s": 93.512,
  "position_beat": 196.0,
  "rate": 1.02,
  "bpm": 126.0,
  "key": "8A",
  "loop": { "active": false, "in_s": null, "out_s": null },
  "on_air": true,
  "audible": 0.87,
  "fader": 1.0,
  "eq": { "low": 0.5, "mid": 0.5, "high": 0.5, "low_kill": false, "mid_kill": false, "high_kill": false },
  "filter": 0.0,
  "pfl": false,
  "scratching": false,
  "slip": { "active": false, "position_s": null }
}
```

| Campo | Tipo | Descripción |
|---|---|---|
| `deck` | int | Número de deck (desde 1) |
| `track_id` | string | Identificador del tema cargado (§10.3). `null` si vacío |
| `loaded` | bool | Hay tema cargado |
| `playing` | bool | Está reproduciendo |
| `position_s` | float | Posición en segundos, válida en el instante `t` |
| `position_beat` | float | Posición en beats según el beatgrid |
| `rate` | float | Velocidad de reproducción (1.0 = original; negativo = hacia atrás) |
| `bpm` | float | BPM efectivo del deck |
| `key` | string | Tonalidad en notación Camelot u Open Key, según se indique en `track.loaded` |
| `loop` | objeto | Estado del loop |
| `on_air` | bool | Es el deck que más se oye |
| `audible` | float 0–1 | Cuánto se oye en la mezcla (fader × crossfader × máster normalizados) |
| `fader` | float 0–1 | Fader de canal |
| `eq` | objeto | EQ normalizado (0.5 = neutro) y "kills" |
| `filter` | float -1–1 | Filtro: negativo = paso bajo, positivo = paso alto, 0 = neutro |
| `pfl` | bool | Escucha previa en cascos activa |
| `scratching` | bool | El DJ está manipulando el plato (scratch, backspin) (§9.4) |
| `slip` | objeto | Slip Mode: si está activo y la posición virtual adonde volverá la música |

> Los receptores **DEBERÍAN** extrapolar la posición: `position_s(t') = position_s + (t' - t)/1e6 × rate`.

### 10.2 `mix.state`

```json
{
  "type": "mix.state",
  "t": 50100000,
  "crossfader": -0.2,
  "on_air_deck": 1,
  "transition": { "active": true, "kind": "blend", "from_deck": 1, "to_deck": 2, "progress": 0.35 }
}
```

| Campo | Descripción |
|---|---|
| `crossfader` | -1 (izquierda) a 1 (derecha) |
| `on_air_deck` | Deck que más se oye |
| `transition.active` | Hay un cambio de deck en curso |
| `transition.kind` | `blend` (mezcla progresiva) o `cut` (cambio brusco) |
| `transition.progress` | 0 = solo `from_deck`, 1 = solo `to_deck` |

El cálculo de `transition` es del emisor. Reglas recomendadas:

- **Inicio inmediato:** `transition.active` pasa a `true` **en cuanto** un segundo deck supera `audible > 0.15` mientras otro sigue audible, sin esperar. Empieza como `kind: blend`.
- **Fin con histéresis:** la transición termina cuando el deck que sale baja de `audible < 0.05` durante 1 beat, o cuando el que entra vuelve a bajar de 0.05 (mezcla abortada).
- `progress = audible_to / (audible_from + audible_to)`.
- **Corte (`cut`):** si el deck en el aire cambia en **menos de ½ beat** (corte de crossfader, cambio brusco de fader), el emisor envía `mix.state` con `kind: cut` y `progress: 1`, y el evento `mix.cut` (Anexo B.4) de inmediato, sin agrupar (§8.4).
- **Chops y gates:** cada vez que un deck audible se corta de golpe (`audible` cae por debajo de 0.05 en menos de 50 ms) o vuelve (sube de 0.05 a más de 0.15), el emisor envía el evento `deck.gate` con `state: closed` u `open`. Así los receptores pueden reaccionar a cada golpe de crossfader en técnicas rápidas, aunque no llegue a haber transición.

### 10.3 `track.loaded`

Se envía al cargar un tema en un deck.

```json
{
  "type": "track.loaded",
  "t": 49000000,
  "deck": 2,
  "track_id": "mixxx:a91b77c0",
  "meta": {
    "title": "Nombre del tema",
    "artist": "Artista",
    "album": "Álbum",
    "genre": "Techno",
    "year": 2024,
    "duration_s": 412.3,
    "bpm": 128.0,
    "key": "5A",
    "key_notation": "camelot",
    "color": "#3A7BFF",
    "rating": 4,
    "tags": ["peak", "dark"],
    "comment": ""
  },
  "artwork": { "url": "http://192.168.1.20:9470/artwork/a91b77c0.jpg", "hash": "sha1:2fd4e1c67a2d28fced849ee1bb76e7391b93eb12" },
  "structure_available": true
}
```

- `track_id` **DEBE** ser estable para el mismo archivo entre sesiones cuando sea posible (por ejemplo, prefijo del emisor + identificador de su biblioteca, o hash del contenido de audio).
- La carátula se sirve por HTTP en el mismo puerto del canal de control. El receptor **PUEDE** ignorarla.
- `structure_available` indica si hay o habrá mapa del tema (perfil 2).

`track.unloaded` (`{ "type": "track.unloaded", "deck": 2 }`) cuando el deck se vacía.

### 10.4 `intent` — capa de intención

Valores de alto nivel para receptores sencillos. Los calcula el emisor o un puente, a partir del deck en el aire, la mezcla y el análisis.

```json
{
  "type": "intent",
  "t": 50100000,
  "energy": 0.78,
  "mood": "dark",
  "valence": 0.3,
  "arousal": 0.85,
  "palette": ["#1B0A3C", "#6A00FF", "#FF2E88"],
  "intensity_hint": "high",
  "strobe_ok": true
}
```

| Campo | Tipo | Descripción |
|---|---|---|
| `energy` | float 0–1 | Energía actual de la música |
| `mood` | enum | Ambiente (Anexo B.2) |
| `valence` | float 0–1 | Triste/oscuro (0) ↔ alegre/luminoso (1) |
| `arousal` | float 0–1 | Calmado (0) ↔ intenso (1) |
| `palette` | string[] | Colores sugeridos en hexadecimal (1 a 6) |
| `intensity_hint` | enum | `low`, `medium`, `high`, `peak` |
| `strobe_ok` | bool | Si es buen momento para efectos agresivos (falso en intros y breakdowns) |

El emisor **DEBERÍA** enviar `intent` al cambiar de sección, de tema o de deck en el aire, y como máximo 10 veces por segundo.

### 10.5 `trigger` — disparos manuales del DJ

Órdenes con nombre que el DJ lanza desde pads o atajos. Cada receptor las interpreta a su manera.

```json
{ "type": "trigger", "t": 50200000, "at": 50200000, "name": "strobe", "state": "on", "value": 1.0, "deck": null }
```

| Campo | Descripción |
|---|---|
| `name` | Nombre del disparo (Anexo B.3). Los propios usan prefijo `custom.` |
| `state` | `on`, `off` o `momentary` |
| `value` | Valor opcional 0–1 (intensidad, velocidad…) |
| `deck` | Deck relacionado, si aplica |

### 10.6 `event` — eventos de actuación

```json
{
  "type": "event",
  "t": 50100000,
  "at": 50250000,
  "event_id": "e-10422",
  "name": "transition.start",
  "deck": 2,
  "data": { "from_deck": 1, "to_deck": 2 }
}
```

| Campo | Descripción |
|---|---|
| `at` | Instante en que ocurre (puede ser futuro, §7.4) |
| `event_id` | Identificador único, para poder cancelarlo |
| `name` | Tipo de evento (Anexo B.4) |
| `deck` | Deck relacionado |
| `data` | Datos propios de cada tipo |

Cancelación de un evento anunciado:

```json
{ "type": "event.cancel", "t": 50120000, "event_id": "e-10422", "reason": "seek" }
```

---

## 11. Perfil 2 — Estructura

### 11.1 `track.structure` — el mapa del tema

Se envía tras `track.loaded` (o cuando termina el análisis). Permite al receptor **conocer el tema entero por adelantado**.

```json
{
  "type": "track.structure",
  "t": 49000500,
  "track_id": "mixxx:a91b77c0",
  "analysis": { "source": "mixxx+vertexav-bridge", "version": "1.0", "confidence": 0.82 },
  "beatgrid": {
    "type": "constant",
    "bpm": 128.0,
    "first_beat_s": 0.112,
    "beats_per_bar": 4,
    "first_downbeat_beat": 0
  },
  "sections": [
    { "start_beat": 0,   "end_beat": 64,  "role": "intro",     "energy": 0.30, "confidence": 0.9 },
    { "start_beat": 64,  "end_beat": 128, "role": "build",     "energy": 0.55, "confidence": 0.8 },
    { "start_beat": 128, "end_beat": 256, "role": "drop",      "energy": 0.95, "confidence": 0.9 },
    { "start_beat": 256, "end_beat": 320, "role": "breakdown", "energy": 0.35, "confidence": 0.7 },
    { "start_beat": 320, "end_beat": 384, "role": "build",     "energy": 0.60, "confidence": 0.8 },
    { "start_beat": 384, "end_beat": 512, "role": "drop",      "energy": 1.00, "confidence": 0.9 },
    { "start_beat": 512, "end_beat": 576, "role": "outro",     "energy": 0.40, "confidence": 0.9 }
  ],
  "energy_curve": { "resolution": "beat", "values": [0.28, 0.29, 0.31, 0.30, 0.33, 0.35, 0.36, 0.38] },
  "markers": [
    { "kind": "hotcue",      "index": 1, "position_s": 0.112,  "label": "Inicio", "color": "#FF0000" },
    { "kind": "hotcue",      "index": 8, "position_s": 60.11,  "label": "Drop",   "color": "#FFAA00", "role": "drop" },
    { "kind": "intro_end",   "position_s": 30.11 },
    { "kind": "outro_start", "position_s": 240.11 }
  ],
  "mood": { "mood": "dark", "valence": 0.3, "arousal": 0.8 }
}
```

**`beatgrid`:**

| `type` | Campos | Uso |
|---|---|---|
| `constant` | `bpm`, `first_beat_s` | Tempo fijo (la mayoría de música electrónica) |
| `variable` | `beats_s`: lista de instantes de cada beat en segundos | Música en directo, tempo cambiante |

**`sections`:** `role` según el Anexo B.1. Las secciones **DEBEN** estar ordenadas y no solaparse. Los huecos se consideran `unknown`.

**`energy_curve`:** `resolution` puede ser `beat`, `bar` o un número de segundos (`0.5`). Valores 0–1. El ejemplo está abreviado: en la práctica hay un valor por cada unidad de resolución de todo el tema.

**`markers`:** hotcues, marcadores de intro/outro y loops guardados. El campo opcional `role` permite que el DJ etiquete un hotcue con un significado (por ejemplo, convención "hotcue 8 = drop").

### 11.2 Prioridad de fuentes

Si hay varias fuentes para la estructura, el orden de prioridad es:

1. Marcadores con `role` puestos por el DJ.
2. Guion editado manualmente (si existe, §17.4).
3. Análisis automático.

### 11.3 `section.change`

Evento anunciado con antelación (§7.4) cuando el deck en el aire entra en una nueva sección.

```json
{
  "type": "event",
  "at": 60110000,
  "event_id": "e-10501",
  "name": "section.change",
  "deck": 2,
  "data": { "from": "build", "to": "drop", "section_index": 2, "energy": 0.95 }
}
```

### 11.4 `countdown` — cuentas atrás

Para preparar momentos clave. El emisor **DEBERÍA** enviar cuentas atrás antes de cada `drop` y de cada cambio de sección con salto de energía mayor de 0.3.

```json
{
  "type": "event",
  "at": 56360000,
  "event_id": "e-10500",
  "name": "countdown",
  "deck": 2,
  "data": { "target": "drop", "beats_remaining": 8, "target_at": 60110000 }
}
```

Valores recomendados de `beats_remaining`: 16, 8, 4, 2, 1.

---

## 12. Perfil 3 — Audio

### 12.1 Dos niveles

| Nivel | Contenido | Transporte | Para quién |
|---|---|---|---|
| **Características** | Niveles, bandas, golpes | UDP (`kind` 0x10) | Todos: luces y visuales |
| **Audio PCM** | Muestras de audio | Memoria compartida (local) o UDP (`kind` 0x20, remoto) | Visualizadores que lo necesiten (projectM) |

El análisis de características se calcula **una sola vez en el emisor** y se reparte a todos.

### 12.2 Configuración

Al suscribirse a `features` o `audio`, el emisor responde con la descripción del flujo:

```json
{
  "type": "features.config",
  "rate_hz": 60,
  "sources": [ { "id": 0, "name": "master" }, { "id": 1, "name": "deck1" }, { "id": 2, "name": "deck2" } ],
  "bands": { "count": 16, "scale": "log", "min_hz": 30, "max_hz": 16000, "edges_hz": [30, 44, 66, 97, 144, 213, 316, 468, 693, 1026, 1519, 2249, 3329, 4930, 7299, 10806, 16000] },
  "pre_fader": true
}
```

- `master` es la mezcla final. `deckN` es la señal de cada deck **antes del fader** si `pre_fader` es `true`, para que los visuales no dependan del volumen.

### 12.3 Paquete `features` (UDP, `kind` 0x10)

Tras la cabecera común:

| Offset | Tamaño | Campo |
|---|---|---|
| 24 | 1 | `source_id` (uint8) |
| 25 | 1 | `band_count` (uint8) |
| 26 | 2 | `onsets` (uint16, bits: 0 = bombo, 1 = caja, 2 = hi-hat, 3 = otro golpe) |
| 28 | 4 | `rms` (float32, 0–1) |
| 32 | 4 | `peak` (float32, 0–1) |
| 36 | 4 | `onset_strength` (float32, 0–1) |
| 40 | 4 × N | `bands[N]` (float32, 0–1, energía normalizada por banda) |

- Un paquete por fuente y por intervalo.
- Con 16 bandas: 104 bytes por paquete.

### 12.4 Audio PCM local (memoria compartida)

El receptor pide el audio:

```json
{ "type": "audio.subscribe", "sources": ["master"], "transport": "shm" }
```

El emisor responde:

```json
{
  "type": "audio.config",
  "transport": "shm",
  "shm_name": "vertexav-8d3a91-master",
  "sample_rate": 48000,
  "channels": 2,
  "format": "f32le",
  "interleaved": true,
  "ring_frames": 65536,
  "block_frames": 256,
  "instance_id": "9f2c4e1a7b3d5c60"
}
```

**Estructura del búfer compartido:**

| Offset | Tamaño | Campo |
|---|---|---|
| 0 | 4 | `magic` `VXAA` |
| 4 | 4 | `version` (uint32) |
| 8 | 4 | `sample_rate` (uint32) |
| 12 | 2 | `channels` (uint16) |
| 14 | 2 | reservado |
| 16 | 4 | `ring_frames` (uint32) |
| 20 | 4 | `block_frames` (uint32): máximo de frames que el emisor escribe antes de publicar |
| 24 | 4 | `header_seq` (uint32, atómico): contador seqlock; impar = el emisor está actualizando la cabecera |
| 28 | 4 | reservado |
| 32 | 8 | `write_frame` (uint64): total de frames publicados desde el inicio |
| 40 | 8 | `write_t` (int64): reloj de show del frame `write_frame` |
| 48 | 8 | `heartbeat_t` (int64, atómico, fuera del seqlock): testigo de vida del emisor (§12.4.1) |
| 56 | 8 | `instance_id` (uint64): número aleatorio que cambia cada vez que el emisor crea el búfer |
| 64 | … | Datos: `ring_frames × channels × 4` bytes, float32 intercalado |

El emisor **nunca espera** al receptor: si un receptor es lento, el emisor sigue escribiendo y sobrescribe datos antiguos. Por eso el receptor **DEBE** comprobar que lo que copió no se sobrescribió mientras lo copiaba.

**Emisor (por cada bloque):**

1. Escribe el bloque en el búfer circular, en las posiciones `[write_frame, write_frame + n)` módulo `ring_frames`, con `n ≤ block_frames`.
2. Incrementa `header_seq` (queda impar), con orden *release*.
3. Actualiza `write_frame += n` y `write_t`.
4. Incrementa `header_seq` (queda par), con orden *release*.

**Receptor (lectura de un rango):**

1. **Cabecera coherente (seqlock):** lee `header_seq` (*acquire*). Si es impar, reintenta. Lee `write_frame` (`W`) y `write_t`. Vuelve a leer `header_seq`. Si cambió, reintenta. Los reintentos están **limitados** (§12.4.1): el receptor nunca espera indefinidamente.
2. **Elige el rango** `[F, F + n)` con `F + n ≤ W` y `F ≥ W - ring_frames + block_frames`. Si se había quedado más atrás, salta hacia adelante y lo cuenta como pérdida.
3. **Copia** los datos a su propia memoria.
4. **Valida después de copiar:** vuelve a leer la cabecera de forma coherente (paso 1) y obtiene `W2`. Si `F < W2 - ring_frames + block_frames`, parte de lo copiado pudo sobrescribirse durante la copia: **descarta la copia** y vuelve al paso 2 con un rango más reciente. Como máximo **3 reintentos** seguidos (§12.4.1).

- El instante de cualquier frame `f` es `write_t - (W - f) × 1 000 000 / sample_rate`.
- Tamaño recomendado: `ring_frames` de al menos **1 s** de audio (65 536 frames a 48 kHz son unos 1,4 s), para que un receptor con pausas cortas no pierda datos.
- Nombre del recurso: en Windows, objeto de memoria con nombre `Local\<shm_name>`; en Linux/macOS, `/<shm_name>` con `shm_open`.

**Audio anticipado:** el emisor **PUEDE** escribir el audio con hasta `lookahead_us` de adelanto respecto a lo que suena, leyendo del tema en memoria. Así el visualizador compensa el retraso de su GPU. `write_t` indica siempre a qué instante del show corresponde.

#### 12.4.1 Robustez: el receptor nunca se bloquea

El emisor puede colgarse, cerrarse de golpe o ser terminado por el sistema en mitad de una escritura y dejar `header_seq` impar para siempre. El receptor **DEBE** seguir funcionando igualmente.

- **Dónde leer:** el receptor **NO DEBE** leer la memoria compartida desde un hilo de audio en tiempo real ni desde un hilo que no pueda permitirse esperar. Lo normal es leer desde el hilo de render o uno dedicado.
- **Espera del seqlock limitada:** entre los dos incrementos de `header_seq` el emisor solo hace unas pocas escrituras, así que un valor impar legítimo dura nanosegundos. El receptor **DEBE** limitar la espera del paso 1 a **50 µs o 1000 iteraciones**, lo que ocurra antes. Si se agota, abandona la lectura en este ciclo, aplica ocultación de pérdidas (§12.5.1) y lo intenta de nuevo en el siguiente ciclo (por ejemplo, el siguiente fotograma).
- **Reintentos por sobrescritura limitados:** como máximo **3** seguidos en el paso 4. Si fallan, el receptor salta a los frames más recientes (`F = W2 - n`), cuenta el salto como pérdida y aplica ocultación de pérdidas.
- **Testigo de vida:** un hilo del emisor que no sea el de audio actualiza `heartbeat_t` con el reloj de show al menos cada **250 ms**, con escritura atómica e independiente del seqlock. Lo hace aunque no suene nada: el emisor sigue escribiendo silencio mientras está en marcha.
- **Emisor caído:** el receptor lo considera caído si ocurre cualquiera de estas cosas:
  - `heartbeat_t` no cambia durante **3 s**, medidos con el reloj local del receptor (el reloj del emisor no sirve si está colgado);
  - `header_seq` se queda en el mismo valor impar más de **100 ms**;
  - se cae el canal de control de la misma sesión (§6.5).
- **Qué hacer si cae:** el receptor **DEBE** soltar la memoria compartida (desmapearla y cerrarla), hacer un fundido a silencio e intentar reconectar (§6.5). Mientras tanto **PUEDE** pasar a capturar la tarjeta de sonido (§12.6), si el usuario lo permitió.
- **Reinicio del emisor:** `instance_id` debe coincidir con el de `audio.config`. Si no coincide, el receptor está leyendo un búfer de otra ejecución y **DEBE** volver a enviar `audio.subscribe`. En JSON va como texto hexadecimal de 16 caracteres, porque los números JSON pierden precisión por encima de 2⁵³.
- **Emisor vivo pero sin audio** (`heartbeat_t` avanza y `write_frame` no): el motor de audio del emisor está detenido. El receptor muestra silencio sin desconectarse.
- **Alineación:** todos los campos atómicos de 64 bits están en posiciones múltiplos de 8, como exigen las operaciones atómicas de la mayoría de procesadores.

### 12.5 Audio PCM remoto (UDP, `kind` 0x20)

Para receptores en otro equipo: `"transport": "udp"`.

Tras la cabecera común:

| Offset | Tamaño | Campo |
|---|---|---|
| 24 | 1 | `source_id` |
| 25 | 1 | `channels` |
| 26 | 2 | `frames` (uint16) en este paquete |
| 28 | 4 | `sample_rate` (uint32) |
| 32 | 8 | `first_frame` (uint64) |
| 40 | … | Muestras: formato `s16le` intercalado (por defecto en red) o `f32le` si se pidió |

- Con `s16le` estéreo y 256 frames por paquete: 1064 bytes por paquete, unos 1,5 Mbit/s a 48 kHz.
- **Sin compresión**, para no añadir retraso.
- El receptor **DEBE** usar un **búfer de jitter** (§12.5.1). Los paquetes de audio no se descartan por llegar desordenados.

#### 12.5.1 Búfer de jitter (receptor)

- **Ordenar por `first_frame`**, no por orden de llegada. Un paquete que llega tarde pero aún no se ha consumido se coloca en su sitio.
- **Profundidad adaptativa:** empezar con 30 ms, medir el jitter de llegada (por ejemplo, percentil 95 de la variación en los últimos 2 s) y ajustar la profundidad entre **10 y 80 ms**. Subir rápido, bajar despacio.
- **Sincronía con el show:** cada frame tiene su instante (`t` + `first_frame`), así que la profundidad del búfer se compensa igual que `latency_us`: el visualizador sigue alineado con las luces aunque el búfer cambie.
- **Ocultación de pérdidas:** un paquete que no llega a tiempo **NO DEBERÍA** rellenarse con silencio puro, porque provoca caídas bruscas en el espectro. Recomendado: repetir el último bloque con un fundido corto (5–10 ms) o interpolar el espectro entre bloques vecinos. Tras 3 pérdidas seguidas, fundido a silencio.
- **Paquetes demasiado tardíos** (su instante ya pasó): se descartan.
- **Duplicados:** un paquete con un `first_frame` que ya está en el búfer se ignora.
- **Vaciado completo:** el receptor **DEBE** vaciar el búfer de jitter y reiniciar sus estimaciones de jitter y de profundidad cuando:
  - recibe un `session_key` nuevo (el emisor se reinició y `first_frame` vuelve a empezar);
  - se reconecta el canal de control;
  - cambia `audio.config` (frecuencia de muestreo, canales o formato).
- **Saltos dentro de la misma sesión:** si `first_frame` salta más de 1 s hacia adelante (hueco largo), el receptor vacía el búfer y continúa desde el nuevo punto. Un salto hacia atrás dentro de la misma sesión no es legítimo: esos paquetes se descartan.
- **Deriva del reloj de audio:** si el búfer crece o se vacía de forma sostenida, el receptor **DEBERÍA** corregir con un remuestreo muy ligero (±0,1 %) en vez de saltar.
- Este audio alimenta visuales, no altavoces: un fallo no se oye, pero sí se ve en el espectro. Por eso conviene ocultarlo.

### 12.6 Plan B: captura de la tarjeta de sonido

Si el emisor no soporta el perfil 3, el receptor **PUEDE** capturar el audio de la salida del sistema (loopback) o de una entrada de línea, y seguir usando los perfiles 0–2 para el tiempo y la estructura. En ese caso el receptor **DEBERÍA** permitir ajustar a mano un retraso de compensación.

---

## 13. Mensajes de retorno (receptor → emisor)

### 13.1 `receiver.state`

Para que el software de DJ pueda mostrar lo que pasa en las luces y los visuales.

```json
{
  "type": "receiver.state",
  "t": 50300000,
  "status": "ok",
  "summary": "Escena: Drop rojo",
  "active": ["strobe"],
  "blackout": false,
  "preset": null
}
```

### 13.2 `receiver.capabilities`

Opcional. Lista de disparos que entiende el receptor, para que el emisor los ofrezca en su interfaz.

```json
{
  "type": "receiver.capabilities",
  "triggers": ["blackout", "strobe", "color.red", "color.blue", "mode.chill", "mode.peak", "custom.laser"]
}
```

### 13.3 `latency.update`

Si el receptor cambia su retraso (por ejemplo, el usuario cambia de interfaz DMX):

```json
{ "type": "latency.update", "latency_us": 32000 }
```

---

## 14. Errores

```json
{ "type": "error", "code": "unsupported_version", "message": "Versión 2.0 no soportada", "fatal": true }
```

| Código | Significado | Fatal |
|---|---|---|
| `unsupported_version` | Versión mayor incompatible | Sí |
| `auth_required` | El emisor exige autenticación y el receptor no la completó | Sí |
| `auth_failed` | Prueba de autenticación incorrecta (§15.4) | Sí |
| `security_downgrade` | El otro lado propone un modo de seguridad más débil que el emparejado | Sí |
| `loop_detected` | Conexión en bucle entre puentes (§5.5) | Sí |
| `message_too_large` | Mensaje mayor que el límite de tamaño o de anidamiento (§15.7) | Sí |
| `bad_message` | JSON mal formado o falta un campo obligatorio | No |
| `unsupported_profile` | Perfil pedido no disponible | No |
| `unsupported_transport` | Transporte de audio no disponible | No |
| `rate_limited` | El receptor envía demasiado | No |
| `internal` | Error interno del emisor | No |

Tras un error fatal, quien lo envía **DEBE** cerrar la conexión.

---

## 15. Seguridad

### 15.1 Modelo de amenazas

Las redes de cabina y escenario a menudo se comparten (Wi-Fi del recinto, técnicos de otras empresas, a veces el público). Se consideran estas amenazas:

- **Escucha pasiva** de la red (Wi-Fi abierto o compartido).
- **Inyección de paquetes UDP falsos** (tempo, eventos) para desincronizar o congelar las luces.
- **Conexión de receptores o emisores no autorizados.**
- **Intermediario activo (MITM)** en la red local.

Fuera de alcance: la confidencialidad de los metadatos musicales (no son secretos) y los ataques con acceso físico a los equipos.

### 15.2 Modos de seguridad

| Modo | Autenticación | Firma UDP | Cifrado | Protege contra | Uso previsto |
|---|---|---|---|---|---|
| `open` | No | No | No | Nada | Solo red cableada privada o mismo equipo |
| `auth` | Desafío-respuesta mutuo; la clave nunca viaja | Sí | No | Escucha pasiva (no obtiene la clave), inyección UDP, conexiones no autorizadas | Red de cabina con dispositivos de terceros |
| `tls` | Igual que `auth`, dentro de `wss://` | Sí | Sí | Todo lo anterior + intermediario activo | Wi-Fi compartido con el público o redes del recinto |

- Todo emisor y receptor **DEBE** implementar `auth`. `tls` **DEBERÍA** implementarse.
- Fuera de `127.0.0.1`, el emisor **DEBERÍA** usar `auth` por defecto. Si funciona en `open` fuera del propio equipo, **DEBE** mostrar un aviso visible al usuario.
- En redes con acceso del público, **DEBE** usarse `tls`.
- **Sin rebajas:** un receptor emparejado con un emisor en `auth` o `tls` **DEBE** negarse a conectarse a ese mismo `id` en un modo más débil (`security_downgrade`).
- En `auth` sin TLS, el contenido del canal de control no va cifrado. Un intermediario activo podría leerlo o manipularlo, pero no puede obtener la clave ni falsificar paquetes UDP. Si esa amenaza importa, usar `tls`.

### 15.3 Emparejamiento y clave compartida

- La clave compartida (PSK) es de **256 bits aleatorios** generados por el emisor con un generador criptográfico. **NO DEBE** ser una contraseña elegida por el usuario: con un desafío capturado se podría adivinar por fuerza bruta.
- Se transfiere **una sola vez**, al emparejar: el emisor muestra un **código QR** o un código de texto (base32, 52 caracteres). En modo `tls`, el código incluye también la **huella SHA-256 del certificado** del emisor, que el receptor fija (*pinning*); así no hacen falta certificados de una autoridad.
- El receptor guarda la clave asociada al `id` del emisor.
- El emisor **DEBERÍA** generar una clave distinta para cada receptor (la identifica por `client_id`). Así puede revocar un receptor sin afectar a los demás.

### 15.4 Desafío-respuesta

```
Receptor                                       Emisor
   │──── hello (client_id; sin secretos) ─────────►│
   │◄──────────────── auth.challenge (server_nonce) │
   │──── auth.response (client_nonce, proof) ─────►│
   │◄──────────────── welcome (server_proof, ...) ──│
```

```json
{ "type": "auth.challenge", "server_nonce": "SHZ+Qs73rcoEx7+bxAucpg==" }
```

```json
{ "type": "auth.response", "client_nonce": "YwJnt0Y45YIouvo4guw71g==", "proof": "kpcc27hJteuQ95yyEGJf5KKA1Lr/AB4C8qEx1afgo/o=" }
```

Los valores binarios van en base64. Con `‖` como concatenación de bytes y los textos en UTF-8:

- `proof = HMAC-SHA256(PSK, "vertexav-auth-client" ‖ server_nonce ‖ client_nonce ‖ client_id)`
- `server_proof = HMAC-SHA256(PSK, "vertexav-auth-server" ‖ client_nonce ‖ server_nonce ‖ session_id)`
- `mac_key = HKDF-SHA256(IKM = PSK, salt = server_nonce ‖ client_nonce, info = "vertexav-udp-mac-v1", L = 32)`

Reglas:

- Los nonces son de **16 bytes** aleatorios criptográficos y no se reutilizan nunca.
- Las pruebas se comparan **en tiempo constante**.
- El receptor **DEBE** verificar `server_proof` antes de aceptar ningún dato: la autenticación es **mutua** y evita emisores falsos.
- Tras **5 fallos** desde la misma dirección, el emisor **DEBE** esperar 30 s antes de aceptar otro intento desde ella.
- `mac_key` es distinta en cada conexión y nunca se transmite.

### 15.5 Firma de paquetes UDP

- En `auth` y `tls`, **todo** paquete UDP, en ambos sentidos (incluido `clock.ping`), **DEBE** llevar una **firma de 8 bytes** en sus últimos 8 bytes (posición `longitud - 8`, ver §8.3): los primeros 8 bytes de `HMAC-SHA256(mac_key, cabecera ‖ contenido)`. El bit 0 de `flags` vale 1.
- El receptor **DEBE** descartar los paquetes sin firma o con firma incorrecta antes de aplicar ninguna otra regla (§8.3). Así un atacante no puede adelantar `seq` ni inyectar tempo falso.
- Coste: calcular la firma de un paquete de unos 100 bytes lleva alrededor de un microsegundo; es despreciable incluso con varios receptores a 60 Hz.
- **Multicast:** con claves por conexión no se puede firmar un paquete para todos. En `tls`, el emisor **PUEDE** entregar en `welcome` una `group_mac_key` para el multicast (viaja cifrada). En `auth`, el multicast **NO DEBE** usarse.

### 15.6 Otras reglas

- El emisor **DEBERÍA** escuchar por defecto solo en la red local, no en interfaces públicas.
- Los receptores **NO DEBEN** ejecutar nada derivado de los campos de texto (títulos, etiquetas): son datos.
- Los paquetes UDP con `session` desconocida **DEBEN** descartarse.
- `session_key` no es un secreto y no aporta seguridad; la protección la dan la firma y la autenticación.

### 15.7 Límites de conexión

Para que muchas conexiones lentas o inactivas no agoten los recursos del emisor, en todos los modos (incluido `open`):

| Límite | Valor |
|---|---|
| Desde que se abre la conexión TCP hasta recibir `hello` (incluida la negociación WebSocket y, si hay, TLS) | **2 s** |
| Desde `auth.challenge` hasta recibir `auth.response` | **3 s** |
| Conexión completa, desde TCP hasta `welcome` | **5 s** |
| Conexiones sin autenticar por dirección IP | **4** |
| Conexiones sin autenticar en total | **32** |
| Tamaño máximo de un mensaje antes de autenticar | **4 KB** |
| Tamaño máximo de un mensaje después de autenticar, del receptor al emisor | **64 KB** |
| Tamaño máximo de un mensaje después de autenticar, del emisor al receptor | **512 KB** (cabe el mapa de un tema muy largo con beatgrid variable) |
| Profundidad máxima de anidamiento JSON | **32 niveles** |
| Tamaño máximo de una carátula (HTTP) | **2 MB** |
| Receptores autenticados | Configurable (por defecto **64**) |

- Si se supera un límite de tiempo o de tamaño antes de autenticar, el emisor cierra la conexión sin responder.
- Los límites de tamaño y anidamiento se aplican **durante toda la sesión y en ambos sentidos**: quien recibe un mensaje que los supera cierra la conexión con `error` `message_too_large`.
- El límite de tamaño **DEBE** comprobarse **antes** de acumular el mensaje en memoria: con la longitud que anuncia la trama WebSocket y sumando los fragmentos a medida que llegan, no después de recibirlo entero.
- Si se llena el cupo de conexiones sin autenticar, el emisor rechaza las nuevas. Las conexiones ya autenticadas **nunca** se cierran para hacer sitio.
- Antes de autenticar, el emisor solo acepta `hello` y `auth.response`; cualquier otro mensaje cierra la conexión.
- El emisor **DEBERÍA** atender las conexiones de forma asíncrona (sin un hilo por conexión), para que los sockets inactivos apenas consuman recursos.

---

## 16. Pasarelas a otros protocolos

Un puente **PUEDE** traducir VertexAV a otros protocolos para software que aún no lo soporte.

### 16.1 OS2L

| VertexAV | OS2L |
|---|---|
| `tempo` (cada beat) | Mensaje `beat` con `change`, `pos`, `bpm`, `strength` |
| `trigger` | Mensaje `btn` con el `name` del disparo y `state` on/off |
| `event` (`section.change`, `drop`) | Mensaje `btn` con nombre `vertexav.section.<rol>` |

Esto permite usar VertexAV hoy con QLC+ (que ya tiene plugin OS2L) y con software comercial compatible con OS2L.

### 16.2 OSC

Espacio de direcciones recomendado:

| Dirección | Argumentos |
|---|---|
| `/vertexav/tempo` | `f bpm`, `f beat`, `i beats_per_bar` |
| `/vertexav/beat` | `i beat_in_bar` (enviado en cada beat) |
| `/vertexav/deck/<n>/playing` | `i 0/1` |
| `/vertexav/deck/<n>/audible` | `f 0–1` |
| `/vertexav/mix/transition` | `i from`, `i to`, `f progress` |
| `/vertexav/section` | `s role`, `f energy` |
| `/vertexav/countdown` | `s target`, `i beats_remaining` |
| `/vertexav/intent/energy` | `f 0–1` |
| `/vertexav/intent/mood` | `s mood` |
| `/vertexav/trigger/<name>` | `i 0/1`, `f value` |
| `/vertexav/features/<source>/bands` | `f…` |

### 16.3 MIDI

- **MIDI Clock** (24 pulsos por negra) y Start/Stop a partir de `tempo`.
- Notas configurables para beat, downbeat, cambios de sección y disparos.
- CC configurables para `energy`, `audible` de cada deck y bandas de frecuencia.

### 16.4 Ableton Link

Ableton Link es colaborativo y **sin maestro**: todos los participantes comparten una línea de tiempo y cualquiera puede proponer cambios de tempo. VertexAV **NO DEBE** intentar imponerse a Link.

- El emisor **PUEDE** unirse a una sesión de Link. Cómo se relacionan sus decks con Link (si siguen a Link o le proponen tempo) lo decide el software de DJ, igual que sin VertexAV.
- **Si el emisor está en una sesión de Link, el tempo y la fase que publica VertexAV (§9) DEBEN reflejar la línea de tiempo de Link** tal como la ve el emisor: mismo BPM y misma fase de compás (el `quantum` de Link equivale a `beats_per_bar`). Así no hay dos verdades.
- Un receptor que participe en Link y reciba VertexAV **DEBE** usar Link para el tempo y la fase, y VertexAV para todo lo demás (decks, estructura, eventos, audio). Los instantes `at` de VertexAV siguen siendo válidos porque describen la misma línea de tiempo.
- Si el deck en el aire **no** está sincronizado con Link (por ejemplo, el DJ desactivó sync y el tema va a otro tempo), el emisor **DEBE** publicar `link_aligned: false` y el tempo real del deck. Los receptores en Link **DEBERÍAN** usar entonces el tempo de VertexAV para las luces, porque es lo que suena, **sin proponerlo** a la sesión de Link.
- VertexAV nunca propone cambios de tempo a Link por su cuenta; solo lo hace el software de DJ, según su configuración.

---

## 17. Guía de implementación

### 17.1 Emisor: Mixxx

- **Regla de oro:** el hilo de audio en tiempo real **NO DEBE** hacer red, JSON, asignaciones de memoria ni bloqueos. Solo copia datos a búferes sin bloqueo (*lock-free*). El análisis y el envío se hacen en otro hilo.
- La información de decks, mezcla y beatgrid ya existe en el motor de Mixxx (controles internos) y en su biblioteca (BPM, tonalidad, hotcues, intro/outro).
- El análisis de estructura (secciones) puede añadirse en Mixxx o delegarse en un **puente** que lea la biblioteca de Mixxx y analice los temas en segundo plano.
- Mixxx y QLC+ usan C++ y Qt: una **librería de referencia en C++/Qt** facilitaría integrarlo en los dos.

### 17.2 Receptor: QLC+

Propuesta de integración:

- **Plugin de entrada VertexAV** que exponga los datos como canales de entrada (beat, downbeat, energía, `audible` por deck, sección actual, disparos), para asignarlos en la consola virtual como cualquier otra entrada.
- **Tempo por beats:** usar `tempo` como fuente de tempo de las funciones (chasers, EFX) en lugar de "Tap", con compensación de `latency_us`.
- **Bancos por sección:** una nueva opción para asociar roles de sección (`intro`, `build`, `drop`, `breakdown`, `outro`) a funciones o colecciones.
- **Colores por intención:** usar `palette` en matrices RGB y efectos de color.

### 17.3 Receptor: projectM

- Leer el audio PCM (memoria compartida o UDP) y pasarlo a projectM con su entrada de muestras.
- Usar `tempo` para sincronizar cambios de preset con el "1" del compás.
- Cambiar de preset en `section.change` y `track.loaded`, y elegir la categoría según `energy` y `mood`.
- Usar `countdown` para lanzar transiciones de preset justo en el drop.
- Mostrar la carátula o el título en las transiciones de tema (opcional).

### 17.4 Puente de guiones (opcional)

Un puente puede añadir guiones de luces por tema:

- Guarda por `track_id` una línea de tiempo de disparos (`trigger`) y escenas en posiciones musicales (beats).
- Genera un guion inicial automático a partir de `track.structure` y permite editarlo.
- Durante la sesión reproduce el guion según la posición del deck en el aire y lo mezcla con el del otro deck según `transition.progress`.

### 17.5 Valores recomendados

| Parámetro | Valor |
|---|---|
| `lookahead_us` | 150 000 (150 ms) |
| Frecuencia de `tempo` UDP | 20 Hz + cada beat |
| Frecuencia de `features` | 60 Hz |
| Bandas de frecuencia | 16 (logarítmicas, 30 Hz – 16 kHz) |
| Bloque de audio PCM | 256 frames |
| Latido | 1 s (caída a los 3 s) |
| Re-sincronización de reloj | Un intercambio UDP cada 1–2 s |
| Valores continuos en el canal de control | Máx. 20 Hz por objeto, zona muerta 0,01 |
| Ventanas de `seq` | Aceptar hasta 256 adelante; descartar hasta 64 atrás; resincronizar con 8 seguidos |
| Firma UDP | 8 bytes (HMAC-SHA256 truncado) |
| Búfer de memoria compartida | ≥ 1 s de audio (65 536 frames a 48 kHz) |
| `hops` máximo entre puentes | 4 |
| Espera máxima del seqlock (receptor) | 50 µs o 1000 iteraciones |
| Reintentos por sobrescritura | 3 |
| `heartbeat_t` | Cada 250 ms; emisor caído tras 3 s sin cambios |
| Tiempos de conexión | `hello` 2 s, `auth.response` 3 s, total 5 s |
| Reconexión de puentes | 0,5 s → 30 s exponencial, ± 25 % |
| Tamaño máximo de mensaje (sesión autenticada) | 64 KB receptor → emisor; 512 KB emisor → receptor |
| Reloj de show | Empieza en 0 al arrancar el emisor |
| Búfer de jitter de audio remoto | 10–80 ms adaptativo (inicio 30 ms) |
| Corrección de fase del receptor | Máx. 2 % del periodo por beat; salto directo si el error > ¼ de beat |

---

## 18. Conformidad

### 18.1 Emisor

| Perfil | Requisitos mínimos |
|---|---|
| 0 | Descubrimiento DNS-SD, `hello`/`welcome`, `clock.ping/pong` por UDP, `snapshot`, `tempo` (control + UDP) con `stability` y `phase_reset`, latido |
| 1 | + `deck.state`, `mix.state`, `track.loaded/unloaded`, `event` + `event.cancel`, `intent`, `trigger`, límites de frecuencia (§8.4) |
| 2 | + `track.structure`, `section.change`, `countdown` |
| 3 | + `features.config` y paquetes `features`; audio PCM por memoria compartida **o** UDP |

### 18.2 Receptor

| Perfil | Requisitos mínimos |
|---|---|
| 0 | Conectarse, sincronizar reloj por UDP, extrapolar el tempo con suavizado, gestionar `unstable` y `phase_reset`, compensar su latencia |
| 1 | + aplicar deltas de estado, ignorar campos y mensajes desconocidos, gestionar `event.cancel` |
| 2 | + usar el mapa del tema para anticipar secciones |
| 3 | + consumir `features` y/o audio PCM (con búfer de jitter si es remoto; con seqlock y validación tras copiar si es local) |

### 18.3 Requisitos comunes

- **Seguridad:** todo emisor y receptor **DEBE** implementar el modo `auth`, incluida la firma UDP. `tls` **DEBERÍA** implementarse.
- **Identidad y topología:** todo emisor **DEBE** tener `id` y anunciar `origin` y `hops`. Todo puente **DEBE** aplicar la prevención de bucles de §5.5.
- **Secuencias:** todo receptor **DEBE** aplicar el orden de validación (incluida la longitud) y las ventanas de `seq` de §8.3.
- **Límites de conexión:** todo emisor **DEBE** aplicar los límites de §15.7.
- **Memoria compartida:** todo receptor que la use **DEBE** aplicar las reglas de robustez de §12.4.1, y todo emisor **DEBE** mantener `heartbeat_t`.

### 18.4 Pruebas

Se propone publicar junto a la especificación:

- Un **emisor de pruebas** que reproduzca una sesión grabada (tempo, decks, estructura, audio).
- Un **receptor de pruebas** que valide los mensajes y mida la precisión del beat.
- Sesiones grabadas de ejemplo en formato JSON Lines.

---

## 19. Cuestiones abiertas

1. **Marca:** registrar "VertexAV Link" (por ejemplo, ante el IMPI en México) y, si se quiere, el nombre de servicio `vertexav` en el registro de IANA para DNS-SD.
2. **Puertos:** registrar o elegir puertos definitivos (9470/9471 son provisionales).
3. **Formato binario del control:** ¿añadir CBOR como alternativa a JSON para receptores embebidos (ESP32, WLED)?
4. **Análisis de estructura:** ¿qué herramienta de referencia usar y cómo normalizar sus roles de sección entre géneros?
5. **Varias cabinas:** cómo coordinar dos emisores en un relevo de DJs (¿quién manda el tempo?).
6. **Licencia de la especificación** (propuesta: CC BY 4.0) y de la librería de referencia (propuesta: MIT o LGPL para que la puedan usar Mixxx, QLC+ y projectM).
7. **Tiempos de vídeo:** ¿añadir un perfil para software de VJ con timecode (SMPTE/MTC)?
8. **Emparejamiento con PIN corto:** permitir un PIN de 6 dígitos en lugar del QR requeriría un protocolo PAKE (por ejemplo, SPAKE2), porque un PIN corto sería vulnerable a fuerza bruta con el desafío-respuesta actual.
9. **Gestión de claves:** formato de exportación y copia de seguridad de emparejamientos, y revocación desde la interfaz del emisor.

---

## Anexo A — Ejemplo de sesión completa

Situación: el DJ tiene un tema sonando en el deck 1, carga otro en el deck 2, mezcla y llega el drop.

```text
# 1. Conexión
R → E  hello            { profiles: [0,1,2], latency_us: 25000, udp_port: 52110 }
E → R  welcome          { profiles: [0,1,2], lookahead_us: 150000 }
R ↔ E  clock.ping/pong  x16 (UDP)
R → E  subscribe        { topics: [tempo, deck, mix, track, structure, event, intent] }
E → R  snapshot         { tempo, decks[1..4], mix, tracks, intent }

# 2. El DJ carga un tema en el deck 2
E → R  track.loaded     { deck: 2, track_id: "mixxx:a91b77c0", meta: {...} }
E → R  track.structure  { track_id: "mixxx:a91b77c0", sections: [...] }

# 3. El DJ pone en marcha el deck 2 y empieza a subir el fader
E → R  deck.state       { deck: 2, playing: true, audible: 0.2 }
E → R  event            { name: "transition.start", data: { from_deck: 1, to_deck: 2 } }
E → R  mix.state        { transition: { active: true, progress: 0.2 } }
...    mix.state        { transition: { progress: 0.5 } }
...    mix.state        { transition: { progress: 0.9 } }
E → R  event            { name: "transition.end", deck: 2 }
E → R  tempo            { source_deck: 2, bpm: 128.0, ... }
E → R  intent           { energy: 0.55, mood: "dark", palette: [...] }

# 4. Se acerca el drop del deck 2 (anunciado con antelación)
E → R  event            { name: "countdown", data: { target: "drop", beats_remaining: 8 } }
E → R  event            { name: "countdown", data: { target: "drop", beats_remaining: 4 } }
E → R  event            { name: "countdown", data: { target: "drop", beats_remaining: 1 } }
E → R  event            { name: "section.change", at: <instante exacto>, data: { to: "drop" } }
E → R  intent           { energy: 0.95, intensity_hint: "peak", strobe_ok: true }

# 5. El DJ lanza un estrobo manual
E → R  trigger          { name: "strobe", state: "momentary" }

# 6. El receptor informa de su estado
R → E  receiver.state   { summary: "Escena: Drop rojo + estrobo" }
```

---

## Anexo B — Enumeraciones

### B.1 Roles de sección (`role`)

| Valor | Descripción |
|---|---|
| `intro` | Inicio del tema, normalmente pensado para mezclar |
| `verse` | Verso / parte principal sin clímax |
| `chorus` | Estribillo |
| `build` | Subida de tensión antes de un drop |
| `drop` | Clímax, máxima energía |
| `breakdown` | Bajada de energía, a menudo sin bombo |
| `bridge` | Parte de transición dentro del tema |
| `outro` | Final del tema, pensado para mezclar |
| `silence` | Silencio |
| `unknown` | No determinado |

### B.2 Ambientes (`mood`)

`dark`, `euphoric`, `happy`, `melancholic`, `aggressive`, `chill`, `dreamy`, `groovy`, `epic`, `neutral`.

### B.3 Disparos estándar (`trigger.name`)

| Nombre | Significado sugerido |
|---|---|
| `blackout` | Apagar todo |
| `fullon` | Todo a tope |
| `strobe` | Estrobo |
| `freeze` | Congelar el estado actual |
| `color.<nombre>` | Color dominante (`red`, `blue`, `green`, `white`, `amber`, `purple`…) |
| `mode.chill` | Modo tranquilo |
| `mode.build` | Modo subida |
| `mode.peak` | Modo máxima energía |
| `fog` | Máquina de humo |
| `fx.<n>` | Efecto genérico número `n` |
| `custom.<nombre>` | Disparo propio |

### B.4 Eventos (`event.name`)

| Nombre | `data` |
|---|---|
| `track.start` | `{}`: un deck empieza a sonar |
| `track.stop` | `{ "kind": "pause" \| "brake" \| "end" }` |
| `transition.start` | `{ "from_deck", "to_deck" }` |
| `transition.end` | `{ "deck" }` |
| `section.change` | `{ "from", "to", "section_index", "energy" }` |
| `countdown` | `{ "target", "beats_remaining", "target_at" }` |
| `seek` | `{ "from_s", "to_s" }`: salto de cue o búsqueda |
| `loop.on` / `loop.off` | `{ "in_s", "out_s", "beats" }` |
| `scratch.start` / `scratch.end` | `{}` |
| `fx.on` / `fx.off` | `{ "name", "amount" }`: eco, reverb, filtro… |
| `sampler.fire` | `{ "slot", "name" }` |
| `bass.kill` / `bass.return` | `{}`: corte o vuelta de graves |
| `mix.cut` | `{ "from_deck", "to_deck" }`: el deck en el aire cambió en menos de ½ beat |
| `deck.gate` | `{ "state": "closed" \| "open" }`: un deck se corta o vuelve de golpe (chops, cortes de crossfader) |
| `source.lost` / `source.restored` | `{}`: un puente perdió o recuperó su fuente (§5.5) |
| `on_air.change` | `{ "from_deck", "to_deck" }`: cambia el deck en el aire (en mezclas y en cortes) |

---

*Fin del borrador 0.6.*
