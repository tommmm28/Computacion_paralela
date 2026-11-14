# Ventas multinacionales en USD con OpenMP

Aplicación C++17 que lee las ventas de las filiales de **Chile, Colombia y Ecuador**,
valida cada transacción, convierte los montos a **USD** con la tasa diaria de la API
autenticada `api.sebastian.cl` y genera el total en USD por **(país, cliente)**.
Usa **OpenMP** para validar, consultar tasas y agregar en paralelo.

- Período procesado: **2023-01-01 a 2025-12-31, inclusive**.
- Conversión: `monto_USD = monto_local / rate` (rate = moneda local por 1 USD, entregada por la API).
- El código **no contiene tasas, tokens ni credenciales**.

> **Docente:** agregar a [`https://github.com/sebasalazar`](https://github.com/sebasalazar) como miembro
> del repositorio (GitHub → *Settings → Collaborators*). Debe hacerlo un propietario del repositorio;
> el programa no lo hace.

---

## Contenido

1. [Estructura del repositorio](#estructura-del-repositorio)
2. [Compilación](#compilación)
3. [Credenciales (sin exponer secretos)](#credenciales-sin-exponer-secretos)
4. [Ejecución](#ejecución)
5. [Formato de entrada](#formato-de-entrada)
6. [Archivos de salida](#archivos-de-salida)
7. [Estrategia de paralelización](#estrategia-de-paralelización)
8. [Uso de la API y manejo de errores](#uso-de-la-api-y-manejo-de-errores)
9. [Pruebas locales](#pruebas-locales)
10. [Rendimiento](#rendimiento)
11. [Decisiones técnicas y supuestos](#decisiones-técnicas-y-supuestos)
12. [Participación del grupo](#participación-del-grupo)

---

## Estructura del repositorio

```
.
├── main.cpp                  # CLI y orquestación del pipeline
├── src/
│   ├── validacion.hpp        # parseo de CSV, UUID, fecha/hora, monto (funciones puras)
│   ├── json_min.hpp          # parser JSON mínimo (RFC 8259), sin dependencias
│   ├── cache_tasas.hpp       # caché sincronizada (país, fecha) → tasa (std::call_once)
│   ├── api.hpp               # libcurl: JWT con mutex, reintentos, timeouts, 401
│   └── procesamiento.hpp     # lectura, validación paralela, duplicados, tasas, agregación
├── tests/
│   ├── unit_tests.cpp        # pruebas unitarias sin red
│   ├── validate.sh           # pruebas de validación con data/ventas_ejemplo.csv
│   ├── mock_api.py           # API simulada local (solo pruebas)
│   └── e2e_mock.py           # prueba extremo a extremo contra la API simulada
├── scripts/
│   ├── benchmark.sh          # tiempos con 1, 2, 4 y 8 hilos; speedup y eficiencia
│   └── generar_datos.py      # genera CSV sintéticos grandes (sin tasas)
├── data/ventas_ejemplo.csv   # filas válidas e inválidas de ejemplo
├── docs/
│   ├── analisis_rendimiento.md # mediciones y análisis de rendimiento
│   └── REQUISITOS.md           # dependencias, entorno y preparación
├── .env.example              # plantilla de credenciales (el .env real está en .gitignore)
├── Makefile
└── README.md               # instrucciones y participación
```

## Compilación

Requisitos: GCC con OpenMP (probado con **GCC 14.2.0**), `libcurl` con headers de desarrollo
y `make`. Python 3 solo se usa para pruebas y para generar datos.
Consulta también la [guía de requisitos e instalación](docs/REQUISITOS.md), incluida la configuración de WSL en Windows.

```sh
# Debian/Ubuntu
sudo apt install g++ make libcurl4-openssl-dev

make                       # genera ./programa
make CXX=g++-14            # para forzar un compilador concreto
```

Comando directo equivalente (el pedido en el enunciado):

```sh
g++ -O2 -std=c++17 -fopenmp main.cpp -lcurl -o programa
```

`main.cpp` incluye los módulos de `src/` (header-only), por eso basta compilar un solo archivo.
El código compila sin advertencias con `-Wall -Wextra -pedantic -Wshadow -Wconversion`.
Si faltan los headers de libcurl, `make` avisa y compila igual, pero solo funciona `--validate-only`.

## Credenciales (sin exponer secretos)

El programa lee las credenciales **solo** desde variables de entorno:

| Variable | Contenido |
|---|---|
| `CPYD_EMAIL` | correo institucional, p. ej. `tu_correo@utem.cl` |
| `CPYD_APIKEY` | RUT con puntos y dígito verificador, p. ej. `12.345.678-5` |
| `CPYD_API_BASE` | *(opcional, solo pruebas)* URL base alternativa; por defecto `https://api.sebastian.cl/cpyd/v1` |

Opción A — en la terminal (no queda en el repositorio):

```sh
export CPYD_EMAIL='tu_correo@utem.cl'
export CPYD_APIKEY='12.345.678-5'
```

Opción B — archivo `.env` local (ignorado por git):

```sh
cp .env.example .env      # editar con sus datos reales
set -a; source .env; set +a
```

Reglas de seguridad aplicadas:
- El JWT se obtiene al inicio con `POST /login/authenticate`, se guarda **solo en memoria**, se
  comparte entre hilos protegido por `std::mutex` y se borra de la memoria al terminar.
- Nunca se imprime el token, ni la clave, ni el cuerpo de la respuesta de autenticación.
- `.gitignore` excluye `.env`, `*.token`, `*credentials*`, salidas CSV y binarios.
- No pasar credenciales como argumentos de línea de comandos (quedarían en el historial y en `ps`).

## Ejecución

```sh
./programa --input ventas.csv --output resumen_ventas_usd.csv --threads 8
```

| Opción | Descripción | Por defecto |
|---|---|---|
| `--input ARCHIVO` | CSV de ventas | `ventas.csv` |
| `--output ARCHIVO` | CSV de resumen | `resumen_ventas_usd.csv` |
| `--threads N` | hilos OpenMP | los disponibles |
| `--validate-only` | solo valida; no usa la API ni credenciales | — |
| `--fallback-days N` | si una fecha no tiene tasa, usar la última tasa anterior (hasta N días atrás) | `0` (desactivado) |
| `--retries N` | reintentos ante errores transitorios (red, timeout, 429, 5xx) | `3` |
| `--timeout S` | timeout por petición HTTP, en segundos | `30` |
| `--delimiter D` | `,` `;` `tab` o `auto` | `auto` |
| `--country-format F` | columna `pais` como `code` (0/1/2) o `name` (Chile…) | `code` |
| `--metrics ARCHIVO` | agrega una fila con los tiempos por fase (usado por el benchmark) | — |
| `--block-mb N` | tamaño del bloque de lectura en MiB (el archivo nunca se carga completo) | `32` |
| `--mem-mb N` | memoria para detectar boletas duplicadas; si no alcanza, se hacen varias pasadas | 50 % de la RAM disponible |

Códigos de salida: `0` éxito (aunque haya filas omitidas), `1` error global (argumentos,
archivo, credenciales, autenticación), `3` había ventas válidas pero ninguna se pudo convertir.

Ejemplo real con los datos de ejemplo, sin red:

```
$ ./programa --input data/ventas_ejemplo.csv --output /tmp/x.csv --threads 4 --validate-only
Hilos OpenMP: 4
Líneas leídas: 32 (datos: 28, vacías/comentarios: 3, encabezado omitido: 1)
Delimitador: ,
Ventas válidas: 9; filas omitidas en validación: 19
  - cantidad de campos distinta de 5: 2
  - país inválido (debe ser 0, 1 o 2): 2
  - UUID de cliente inválido: 1
  - UUID de boleta inválido: 1
  - fecha/hora con formato inválido: 2
  - fecha/hora inexistente en el calendario: 2
  - fecha fuera del rango 2023-01-01..2025-12-31: 2
  - monto no numérico: 3
  - monto no positivo: 2
  - boleta duplicada: 2
Pares (país, fecha) únicos a consultar: 8
```

## Formato de entrada

Una venta por línea, 5 campos en este orden:

| # | Campo | Tipo | Validación |
|---|---|---|---|
| 1 | País | entero | `0` Chile, `1` Colombia, `2` Ecuador |
| 2 | Cliente | UUID | formato `8-4-4-4-12` hexadecimal; puede repetirse en otro país |
| 3 | Boleta | UUID | formato `8-4-4-4-12`; **única en todo el archivo** |
| 4 | Fecha/hora local | texto | `YYYY-MM-DD HH:MM:SS` o `YYYY-MM-DDTHH:MM:SS` (fracción `.fff` opcional); fecha existente en el calendario; dentro de 2023-01-01..2025-12-31 |
| 5 | Monto | decimal | dígitos con punto decimal opcional, finito y **> 0** |

```
0,f38b2ffc-80a4-4f5a-91c9-bc701e7ea419,7dabe929-c4a3-44bf-86cd-75e9bb049a79,2025-06-20 13:45:00,94058.00
1,f38b2ffc-80a4-4f5a-91c9-bc701e7ea419,1919e93a-d117-45ad-8988-93101c593af5,2025-06-20T14:00:00,4200000
```

Tolerancias: encabezado opcional en la primera línea (se detecta y omite), líneas vacías y
comentarios `#` ignorados, espacios alrededor de los campos, finales de línea `\r\n`, BOM UTF-8,
campos entre comillas dobles y delimitador `;` o tabulador (detección automática).

`data/ventas_ejemplo.csv` contiene 9 filas válidas y 19 inválidas que cubren cada regla.

## Archivos de salida

**`<output>`** — resumen, ordenado por país y luego por id de cliente, 2 decimales:

```
pais,id_cliente,total_usd
0,<uuid-cliente>,<total>
1,<uuid-cliente>,<total>
```

La columna `pais` usa el mismo código del archivo de entrada (0, 1, 2); con
`--country-format name` se escribe `Chile`, `Colombia` o `Ecuador`. Los UUID se escriben en
minúsculas (forma canónica).

**`<output>.errors.csv`** — una fila por venta omitida, en orden de línea:

```
linea,motivo,contenido
16,boleta duplicada: primera aparición en línea 3,"0,e5121482-...,2025-06-20 13:45:00,500"
19,monto no positivo,"1,e5121482-...,2025-06-20 14:00:00,-1"
```

**`<output>.tasas.csv`** — bitácora de cada tasa usada (`pais,fecha,estado,rate,fecha_rate,detalle`):
permite auditar que todas provienen de la API y ver qué fechas no tuvieron tasa.

## Estrategia de paralelización

El cuello de botella real son las **peticiones HTTP** (latencia de red), seguido por el
parseo de archivos grandes. Para que la memoria no dependa del tamaño del archivo, éste
**se lee por bloques de 32 MiB en dos pasadas** (nunca se carga completo): con 10 millones
de filas (1 GB) el programa usa ~490 MB de RAM, contra ~2,5 GB si cargara todo.

| # | Etapa | Paralelismo | Por qué no hay condiciones de carrera |
|---|---|---|---|
| 1a | **Pasada 1**: lectura de un bloque | secuencial (E/S) | — |
| 1b | Parseo y validación por fila | `omp for schedule(static)` | cada iteración escribe solo `filas[i]`; buffers privados por hilo |
| 1c | Boletas duplicadas | región paralela, **partición por fragmentos de hash** | el hilo `t` es dueño exclusivo de las tablas `f % T == t`; cada boleta cae en un solo fragmento; las tablas persisten entre bloques |
| 1d | Conteos y pares `(país, fecha)` | tramos contiguos con **`reduction(+: conteo[:M])`** y **`reduction(\|: marcas[:N])`** | cada hilo usa copias privadas; OpenMP combina al final |
| 2 | Autenticación JWT | una vez, antes de los hilos | — |
| 3 | Consulta de tasas únicas | `omp for schedule(dynamic, 1)` | caché con `std::call_once` por clave; un handle CURL por hilo; token con `std::mutex`; contadores `std::atomic` |
| 4a | **Pasada 2**: lectura de un bloque | secuencial (E/S) | — |
| 4b | Parseo, clasificación y conversión a USD | tramos contiguos por hilo | cada hilo escribe solo las posiciones de su tramo y su propia lista de incidencias |
| 4c | Suma por cliente | **64 mapas particionados por hash del cliente**; el hilo `t` es dueño de los mapas `g % T == t` | cada mapa lo modifica un único hilo; no hay copias por hilo ni secciones críticas |
| 4d | Escritura de incidencias del bloque | secuencial, en orden de línea | ocurre después de la barrera implícita |
| 5 | Unión de los 64 mapas (disjuntos), orden y escritura | **secuencial** | después de la última región paralela |

Detalles y justificación:

- **Deduplicación de consultas.** Todas las ventas de un mismo país y día comparten tasa.
  En vez de una petición por venta se hace una por par `(país, fecha)`: como máximo
  3 × 1.096 = 3.288 peticiones, sin importar si el archivo tiene mil o diez millones de filas.
- **`schedule(dynamic, 1)` en las tasas.** La latencia de cada petición varía mucho (red,
  reintentos, timeouts); el reparto dinámico evita que un hilo quede con varias peticiones
  lentas mientras otros están ociosos. Es trabajo *I/O-bound*: usar más hilos que núcleos
  sigue siendo útil porque los hilos pasan la mayor parte del tiempo esperando.
- **Caché sincronizada.** `CacheTasas` es un arreglo fijo indexado por `(país, día)`. Cada
  entrada tiene un `std::once_flag`: si dos hilos piden la misma fecha (posible durante el
  fallback), solo uno hace la petición y el otro espera el resultado. No hay un mutex global,
  así que claves distintas nunca compiten entre sí.
- **Conexiones reutilizables.** Cada hilo crea un handle CURL al entrar a la región paralela y
  lo reutiliza (keep-alive): se evita repetir el handshake TLS en cada consulta.
- **Token compartido.** `GestorToken` guarda el JWT con un mutex y un contador de generación.
  Si varios hilos reciben 401 al mismo tiempo, solo el primero re-autentica; los demás ven
  que la generación cambió y reintentan con el token nuevo (una sola re-autenticación).
- **`schedule(static)` en validación y suma.** El costo por fila es uniforme, así que bloques
  estáticos dan buen balance con mínimo overhead y conservan la localidad de memoria.
- **Mapas particionados en lugar de `critical`/`atomic`.** Una sección crítica por venta
  serializaría el bucle. Con mapas privados por hilo, cada hilo tendría una copia de casi
  todos los clientes (memoria × número de hilos). Por eso se usan 64 mapas particionados por
  hash del cliente, cada uno con un solo hilo dueño: sin sincronización, sin duplicar memoria y
  sin fusión final.
- **Archivos más grandes que la RAM** (el `ventas.csv` real pesa ~14 GB). Lo único que crece
  con el archivo es la tabla de boletas (~48 bytes por fila). El programa estima su tamaño y,
  si supera el 50 % de la memoria disponible (o `--mem-mb`), detecta los duplicados en K
  pasadas, cada una guardando 1/K de las boletas (las de K grupos de fragmentos). El resultado
  es idéntico a una sola pasada (probado con K = 1, 2, 8 y 64).
- **Dos pasadas en vez de guardar las ventas.** Guardar todas las ventas válidas hasta tener las
  tasas costaría ~150 bytes por fila. En cambio, la pasada 1 solo conserva las boletas vistas
  (~40 bytes por fila, necesarias para detectar duplicados en todo el archivo) y la pasada 2
  vuelve a parsear cada bloque, lo que cuesta poco frente a las consultas HTTP.
- **Determinismo.** Las sumas se acumulan en `long double` y el orden de salida es fijo, por lo
  que el resumen es idéntico con 1, 2, 4 u 8 hilos y con cualquier tamaño de bloque (verificado en
  las pruebas y el benchmark).

## Uso de la API y manejo de errores

| Situación | Política |
|---|---|
| Fila con formato inválido (país, UUID, fecha, monto, nº de campos) | se registra en `<output>.errors.csv` y se omite |
| Boleta repetida | se conserva la **primera** aparición; las siguientes se registran y omiten |
| Fecha fuera de 2023-01-01..2025-12-31 | se registra como incidencia y se omite |
| HTTP 404/204 o respuesta sin `rate` | **sin tasa**: se registran las ventas afectadas y se omiten (ver abajo) |
| HTTP 401 en una consulta | se refresca el JWT **una vez** (con mutex) y se reintenta |
| 401 persistente / fallo al refrescar | error de API para esa fecha; ventas registradas y omitidas |
| Timeout, error de conexión, HTTP 408/429/5xx | reintento con backoff exponencial (250 ms, 500 ms, 1 s… máx. 8 s; respeta `Retry-After`), hasta `--retries` veces; si persiste, error de API |
| Otro HTTP 4xx, JSON inválido, `rate` ≤ 0 o no numérico | error de API para esa fecha; ventas registradas y omitidas |
| Credenciales ausentes o rechazadas en el login inicial | **error global**: el programa termina con código 1 |

Timeouts: 10 s para conectar y 30 s por petición (configurable con `--timeout`).

**Fechas sin tasa disponible.** Política por defecto: *si la API no entrega tasa para la
fecha, la venta se registra en el archivo de errores y se omite*. En los datos reales la API
no tiene tasa para fines de semana ni feriados (951 de 3.288 fechas), por lo que el resultado
entregado usa `--fallback-days 5`. Opcionalmente,
`--fallback-days N` usa **la última tasa anterior disponible**, consultándola a la API día por
día hacia atrás (hasta N días); la fecha efectivamente usada queda en `<output>.tasas.csv`.
El fallback solo se aplica a "sin tasa", nunca a errores de red/servidor. En ningún caso se
inventan, interpolan ni hardcodean tasas.

## Pruebas locales

Ninguna prueba necesita credenciales ni acceso a internet:

```sh
make test          # = make unit validate e2e
```

| Prueba | Qué cubre | Resultado |
|---|---|---|
| `make unit` (`tests/unit_tests.cpp`) | UUID, fechas (bisiestos, límites del rango, formatos), montos (negativos, cero, NaN, inf, notación científica, desbordes), CSV con comillas, encabezados, delimitadores, parser JSON, duplicados con 1–8 hilos y bloques de 1 byte a 32 MiB (líneas cortadas entre bloques), tabla hash con crecimiento, caché concurrente (16 hilos → 1 carga por clave), pipeline completo con consultor falso | 102249/102249 OK |
| `make validate` (`tests/validate.sh`) | cada fila inválida de `data/ventas_ejemplo.csv` con su motivo y línea, duplicados, mismo archivo de errores con 1/3/8 hilos, `;` + CRLF, errores globales | OK |
| `make e2e` (`tests/e2e_mock.py`) | contra la **API simulada** local: totales vs. cálculo independiente en Python, resultados idénticos con 1/2/4/8 hilos, una sola consulta exitosa por `(país, fecha)`, token expirado → exactamente 1 re-autenticación, 503 → reintento, 404 → omitida, fallback, timeout, JSON inválido, tasa 0, credenciales malas/ausentes, el token nunca aparece en la salida | OK |

La API simulada (`tests/mock_api.py`) entrega **tasas sintéticas de prueba** calculadas con una
fórmula; existen solo para verificar el programa y nunca se usan fuera de las pruebas. También
se puede levantar a mano para probar sin credenciales:

```sh
python3 tests/mock_api.py --puerto 8099 &
CPYD_API_BASE=http://127.0.0.1:8099/cpyd/v1 CPYD_EMAIL=prueba@utem.cl CPYD_APIKEY=11.111.111-1 \
  ./programa --input data/ventas_ejemplo.csv --output /tmp/prueba.csv --threads 4
```

## Rendimiento

**Benchmark con la API real y el `ventas.csv` entregado** (14 GB, 119.115.485 ventas;
Intel Core i5-12500, 6 núcleos / 12 hilos, Ubuntu en WSL2):

```sh
make CXX=g++-14
EXTRA="--fallback-days 5" scripts/benchmark.sh ventas.csv benchmark-results/api-real
```

| Hilos (p) | Tiempo [s] | Speedup T1/Tp | Eficiencia S/p |
|---:|---:|---:|---:|
| 1 | 821.00 | 1.000 | 1.000 |
| 2 | 473.04 | 1.736 | 0.868 |
| 4 | 272.53 | 3.013 | 0.753 |
| 8 | 185.24 | 4.432 | 0.554 |

| Fase | 1 hilo [s] | 8 hilos [s] | Speedup |
|---|---:|---:|---:|
| Consulta de tasas (3.294 peticiones HTTP) | 539.8 | 68.8 | 7.85 |
| Validación + duplicados | 136.6 | 35.3 | 3.88 |
| Conversión y suma | 99.6 | 34.3 | 2.91 |
| Lectura + escritura (secuenciales) | 44.7 | 46.7 | ≈ 1 |

- La fase de API, limitada por latencia de red (~164 ms por petición), escala casi
  linealmente: los hilos solapan esperas. El servidor no limitó la tasa de peticiones (0 reintentos).
- Las fases de CPU escalan menos: 6 núcleos físicos (con 8 hilos se usa hyper-threading),
  mapas hash con 25 millones de clientes (accesos aleatorios a memoria) y partes secuenciales.
- Lectura y escritura del disco (~5,5 % del tiempo secuencial) no se paralelizan: por la ley de
  Amdahl el speedup máximo con 8 hilos sería ≈ 5,8.
- Las 4 corridas generaron archivos idénticos. Análisis completo en
  [`docs/analisis_rendimiento.md`](docs/analisis_rendimiento.md).

### Resultados con los datos reales

| Dato | Valor |
|---|---:|
| Líneas del archivo | 119.115.486 (1 encabezado, separador `;`) |
| Ventas válidas | 119.115.485 (0 inválidas, 0 boletas duplicadas) |
| Pares `(país, fecha)` consultados | 3.288 (+6 fechas anteriores para fallback) |
| Fechas sin tasa en la API | 951 (939 sábados/domingos + 12 feriados) |
| Ventas convertidas a USD | 119.115.485 (0 omitidas, con `--fallback-days 5`) |
| Pares `(país, cliente)` en el resumen | 25.357.072 |

Sin fallback (política por defecto) las 951 fechas sin tasa dejan **33.688.393 ventas (28 %)
omitidas** y registradas en `<output>.errors.csv`. Como la API no publica tasas en fines de
semana ni feriados, el resumen entregado se generó con `--fallback-days 5`: esos días usan la
tasa del último día hábil anterior, también obtenida de la API (queda registrado en
`<output>.tasas.csv`).

Mediciones locales complementarias (máquina de desarrollo con **2 núcleos**, GCC 14.2.0):

- **Validación sin red**, 2.000.000 filas: 2,57 s → 1,83 s con 2 hilos (S = 1,40).
  Con 4 u 8 hilos casi no mejora porque solo hay 2 núcleos (eficiencia 0,38 y 0,19).
- **API simulada con 20 ms de latencia artificial**, 3.288 consultas: 69,1 s → 8,8 s con 8 hilos
  (S = 7,87; E = 0,98). Al ser espera de red, 8 hilos rinden incluso con 2 núcleos.
  *Es un experimento de apoyo, no un resultado de la API real.*

**La red/API puede limitar el speedup**: si el servidor aplica límites de tasa (HTTP 429), atiende
pocas conexiones simultáneas o su latencia crece con la carga, la fase de tasas escala peor. En la
medición real la API respondió bien con 8 conexiones simultáneas (speedup 7,85 en esa fase). Además, la lectura del archivo, la autenticación y la fusión final son
secuenciales y acotan la ganancia (ley de Amdahl).

## Decisiones técnicas y supuestos

- **JSON:** se implementó un parser mínimo pero completo (`src/json_min.hpp`) en lugar de
  `nlohmann/json`, para no depender de una biblioteca que podría no estar instalada al corregir.
  Parsea el documento entero (detecta respuestas truncadas o HTML), busca `jwt` y `rate` aunque
  estén anidados y acepta `rate` como número o como texto numérico.
- **UUID:** se exige el formato `8-4-4-4-12` hexadecimal, sin exigir versión/variante RFC 4122
  (los generadores de datos de prueba no siempre las respetan). Mayúsculas y minúsculas se
  consideran el mismo UUID; se escriben en minúsculas.
- **Boletas duplicadas:** la unicidad se controla sobre todo el archivo, incluyendo filas
  inválidas por otro motivo (el enunciado dice "valor único en todo el archivo"). Gana la primera
  aparición.
- **Fechas:** la fecha/hora es local de cada país; para la tasa solo se usa la parte `YYYY-MM-DD`.
  Se rechazan fechas inexistentes (2023-02-29, mes 13, 24:00:00).
- **Montos:** decimal con punto, sin separador de miles ni notación científica (`1e5` se rechaza
  por ambigua en datos contables). Se admite `+` inicial. Cero y negativos se rechazan.
- **Ecuador** usa el dólar como moneda; igualmente se consulta la tasa a la API (no se asume 1).
- **Precisión:** sumas en `long double`; redondeo a 2 decimales solo al escribir.
- **Errores HTTP 404/204** se interpretan como "sin tasa para esa fecha"; otros 4xx como error.
- **Tamaño máximo** de una respuesta HTTP: 1 MiB (protección ante respuestas anómalas).
- Compilado y probado con GCC 14.2.0 (Ubuntu 24.04) y libcurl 8.5.0.

## Participación del grupo

| Integrante | Usuario GitHub | Parte a cargo | Archivos | Commits (hash / enlace) |
|---|---|---|---|---|
| Manuel Dios | [@NeoManD10](https://github.com/NeoManD10) | **Validación de datos y pruebas locales**: parseo del CSV, UUID, fecha/hora, montos, rango de fechas, datos de ejemplo, generador de datos y pruebas sin red | `src/validacion.hpp`, `data/ventas_ejemplo.csv`, `tests/unit_tests.cpp`, `tests/validate.sh`, `scripts/generar_datos.py` | [9d2fcd8](https://github.com/tommmm28/Computacion_paralela/commit/9d2fcd80fa9bb207755cc757533ed4fa43f59cc3) |
| Maximiliano Parra | [@maxparra21](https://github.com/maxparra21) | **API, autenticación y caché de tasas**: cliente libcurl, JWT compartido con mutex, refresco ante 401, reintentos y timeouts, parser JSON, caché sincronizada, manejo seguro de credenciales y API simulada | `src/api.hpp`, `src/json_min.hpp`, `src/cache_tasas.hpp`, `tests/mock_api.py`, `tests/e2e_mock.py`, `.gitignore`, `.env.example` | [a3f6656](https://github.com/tommmm28/Computacion_paralela/commit/a3f665625) · [4a2407e](https://github.com/tommmm28/Computacion_paralela/commit/4a2407e6d) |
| Tomas Martinez | [@tommmm28](https://github.com/tommmm28) | **Paralelización OpenMP, integración y rendimiento**: validación/duplicados en paralelo, reducción de claves, consulta paralela de tasas, agregación con mapas por hilo, programa principal, Makefile, benchmark y análisis de speedup | `src/procesamiento.hpp`, `main.cpp`, `Makefile`, `scripts/benchmark.sh`, `docs/analisis_rendimiento.md`, `README.md` | [1c73ee1](https://github.com/tommmm28/Computacion_paralela/commit/1c73ee17b85868c786d7abce82c99cbe9c1063f9) |

Los enlaces anteriores muestran commits representativos de cada integrante. Para revisar todo el
historial de contribuciones, utiliza `git shortlog -sne main` y los Pull Requests fusionados (#1, #2 y #4).

Recordatorio: agregar al docente [`https://github.com/sebasalazar`](https://github.com/sebasalazar)
como miembro del repositorio.
