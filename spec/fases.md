El plan de Gemini va bien encaminado, pero cambiaría el orden: primero el emisor de pruebas y una pasarela a OS2L, después la librería en C++. Así ves tus luces reaccionando en días, no en meses, y validas la especificación antes de escribir código difícil de cambiar.

Orden que propongo

Fase 1: emisor de pruebas + inspector (Python, días)

Un emisor simulado que se anuncie en la red, abra los puertos 9470/9471 y simule una sesión: tempo, dos decks, una mezcla, un drop con su cuenta atrás. Perfiles 0 a 2.
Un inspector: un receptor que se conecta, muestra todo lo que llega y valida cada mensaje contra la especificación. Será tu banco de pruebas para todo lo demás.
Al empezar en modo open y en el mismo PC, la seguridad (auth) se añade cuando lo básico funcione.

Programar esto contra la especificación descubrirá huecos, y es mejor encontrarlos ahora que en el código de Mixxx.

Fase 2: pasarela a OS2L (la victoria rápida)
QLC+ ya entiende OS2L. Una pasarela que traduzca VertexAV Link a OS2L (beat, drops, secciones como botones) te permite ver tus luces reaccionando sin escribir el plugin de QLC+ todavía. Con eso puedes probarlo en un evento real muy pronto.

Fase 3: libvertexav (C++/Qt)
Aquí entra la Fase 1 de Gemini. Para entonces la especificación ya estará probada y tendrás el inspector para verificar la librería.

Licencia: recomiendo MIT, que es compatible con Mixxx (GPL), QLC+ (Apache 2.0) y projectM (LGPL). Así nadie tiene excusa para no integrarla.

Fase 4: plugin de QLC+ con la librería: tempo sin tap, bancos de escenas por sección y colores por intención.

Fase 5: Mixxx

Empezar con una rama propia (fork) de Mixxx con el emisor. Es lo más difícil y lo más lento de aceptar en el proyecto oficial.
En paralelo, desde ya: presentar la idea a los desarrolladores de Mixxx (su chat Zulip) y de QLC+ (foro). Que lo conozcan antes de recibir el código aumenta mucho las posibilidades de que lo acepten, y puede que te den ideas o te avisen de obstáculos.
Organización

Un repositorio, u organización de GitHub vertexav, con:

spec/: la especificación;
tools/: emisor de pruebas, inspector y pasarela OS2L;
libvertexav/: la librería;
integrations/: el plugin de QLC+ y el parche de Mixxx