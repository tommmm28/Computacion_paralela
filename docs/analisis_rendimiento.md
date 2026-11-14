# Análisis de rendimiento

Este documento separa con claridad tres tipos de resultados:

| Sección | Qué mide | Estado |
|---|---|---|
| [1. API real](#1-api-real-resultado-oficial-de-la-tarea) | Pipeline completo contra `api.sebastian.cl` con el `ventas.csv` real (14 GB) | Medido (i5-12500, 12 hilos lógicos) |
| [2. Validación local](#2-fase-de-validación-sin-red-medición-real) | Lectura + validación + duplicados, sin red | Medido (sandbox de desarrollo, 2 núcleos) |
| [3. API simulada](#3-api-simulada-con-latencia-artificial-experimento-de-apoyo) | Pipeline completo contra un servidor local con 20 ms de latencia artificial | Medido; **no reemplaza** la medición real |

Definiciones: `T1` = tiempo con 1 hilo, `Tp` = tiempo con `p` hilos,
**speedup** `S(p) = T1 / Tp`, **eficiencia** `E(p) = S(p) / p`.

---

## 1. API real (resultado oficial de la tarea)

**Medición real** con el archivo `ventas.csv` entregado (14 GB, 119.115.485 ventas) y la API
`api.sebastian.cl`, ejecutada el 2026-11-14 con:

```sh
make CXX=g++-14
EXTRA="--fallback-days 5" scripts/benchmark.sh ventas.csv benchmark-results/api-real
```

Entorno: Intel Core i5-12500 (12th Gen; 6 núcleos físicos, 12 hilos lógicos, `nproc = 12`),
Ubuntu en WSL2 (Windows), 1 repetición por cantidad de hilos.

| Hilos (p) | Tiempo total [s] | Speedup T1/Tp | Eficiencia S/p |
|---:|---:|---:|---:|
| 1 | 821.00 | 1.000 | 1.000 |
| 2 | 473.04 | 1.736 | 0.868 |
| 4 | 272.53 | 3.013 | 0.753 |
| 8 | 185.24 | 4.432 | 0.554 |

Las cuatro corridas produjeron **exactamente los mismos archivos** (resumen, errores y tasas):
el resultado no depende del número de hilos.

### Tiempo por fase (`metricas_programa.csv`)

| Fase | 1 hilo [s] | 2 hilos [s] | 4 hilos [s] | 8 hilos [s] | Speedup con 8 |
|---|---:|---:|---:|---:|---:|
| Lectura del disco (secuencial) | 37.8 | 48.5 | 38.8 | 39.7 | ≈ 1 |
| Validación + duplicados (2 pasadas de detección + pasada 1) | 136.6 | 79.0 | 46.8 | 35.3 | 3.88 |
| Consulta de tasas a la API (3.294 peticiones) | 539.8 | 269.2 | 137.0 | 68.8 | **7.85** |
| Conversión y suma por cliente (pasada 2) | 99.6 | 68.5 | 41.7 | 34.3 | 2.91 |
| Escritura (secuencial) | 6.9 | 7.5 | 7.9 | 7.0 | ≈ 1 |
| **Total** | **820.9** | **472.9** | **272.4** | **185.1** | **4.43** |

Resultado del procesamiento: 119.115.485 ventas válidas (0 filas inválidas ni duplicadas),
3.288 pares `(país, fecha)`: 2.337 con tasa directa y 951 sin tasa (fines de semana y feriados)
resueltos con la tasa del día hábil anterior; 0 ventas omitidas; 25.357.072 pares
`(país, cliente)` en el resumen. Sin reintentos ni refrescos de token.

### Análisis

- **La consulta a la API escala casi linealmente** (7,85 con 8 hilos, eficiencia 0,98). Con 1
  hilo cada petición tarda en promedio 539,8 s / 3.294 ≈ 164 ms, casi todo espera de red. Con
  8 hilos se solapan 8 esperas, y el servidor respondió sin degradarse (sin HTTP 429 ni
  reintentos). Sin la caché por `(país, fecha)` se habrían necesitado 119 millones de
  peticiones en vez de 3.294.
- **Las fases de CPU escalan menos** (3,88 y 2,91 con 8 hilos):
  - el i5-12500 tiene 6 núcleos físicos; con 8 hilos dos de ellos comparten núcleo
    (hyper-threading), lo que rinde bastante menos que un núcleo extra;
  - la suma por cliente accede a mapas hash con 25 millones de entradas: son accesos
    aleatorios a memoria, limitados por las fallas de caché y el ancho de banda de RAM;
  - cada bloque incluye una parte secuencial (repartir duplicados, escribir incidencias en orden).
- **Ley de Amdahl:** lectura y escritura (≈ 45 s, 5,5 % del tiempo con 1 hilo) no se
  paralelizan. Aun con fases paralelas perfectas, el speedup máximo con 8 hilos sería
  1 / (0,055 + 0,945/8) ≈ 5,8; el real fue 4,43.
- **Memoria:** el archivo pesa 14 GB y WSL tenía ~7 GB disponibles. El programa leyó por
  bloques de 32 MiB y detectó los duplicados en 2 pasadas (tabla estimada en ~5,5 GB > 3,6 GB
  de presupuesto), sin quedarse sin memoria. La versión inicial, que cargaba todo el archivo,
  fallaba con `std::bad_alloc`.
- La eficiencia baja al aumentar los hilos (0,87 → 0,75 → 0,55), como se espera cuando
  crecen las partes secuenciales relativas y la competencia por memoria.

---

## 2. Fase de validación sin red (medición real)

Entorno: Intel Xeon @ 2.10 GHz, **2 núcleos** (`nproc = 2`), GCC 14.2.0, `-O2`.
Archivo sintético de 2.000.000 filas (209 MB) generado con
`scripts/generar_datos.py --filas 2000000 --semilla 11`; 3 repeticiones, mediana.

```sh
python3 scripts/generar_datos.py --filas 2000000 --semilla 11 --salida data/ventas_grande.csv
MODO=validacion REPETICIONES=3 scripts/benchmark.sh data/ventas_grande.csv
```

Tiempo de pared del proceso completo (con `--validate-only` el programa hace las dos pasadas:
la pasada 1 valida y la pasada 2 escribe el archivo de errores; la lectura del disco, ≈ 0,29 s
en total, es secuencial):

| Hilos (p) | Tiempo mediano [s] | Speedup T1/Tp | Eficiencia S/p |
|---:|---:|---:|---:|
| 1 | 2.5682 | 1.000 | 1.000 |
| 2 | 1.8344 | 1.400 | 0.700 |
| 4 | 1.6998 | 1.511 | 0.378 |
| 8 | 1.6626 | 1.545 | 0.193 |

Por fase (`metricas_programa.csv`, medianas): pasada 1 (validación + duplicados) 1,21 s → 0,84 s
con 2 hilos; pasada 2 1,04 s → 0,64 s con 2 hilos (S ≈ 1,6).

Lectura:
- Con 2 núcleos físicos, usar 4 u 8 hilos no puede acelerar trabajo de CPU: los
  hilos extra solo agregan cambios de contexto (por eso la eficiencia cae a la mitad
  cada vez que se duplica `p`, y 8 hilos es levemente más lento que 2).
- La lectura del archivo (≈ 0,29 s) es secuencial: por la ley de Amdahl limita el
  speedup total del proceso aunque la validación escale.
- En una máquina con 8 núcleos se espera que la fase de validación siga escalando,
  porque cada fila se procesa de forma independiente.

### Memoria (lectura por bloques)

Pico de memoria (`/usr/bin/time -f %M`) con `--validate-only`, 2 hilos:

| Filas (tamaño) | Cargando todo el archivo (versión inicial) | Lectura por bloques (versión final) |
|---:|---:|---:|
| 2.000.000 (209 MB) | 580 MB | 254 MB |
| 10.000.000 (998 MB) | 2.572 MB | 488 MB |
| 10.000.000 con `--mem-mb 64` (8 pasadas de duplicados) | — | 226 MB |

La versión inicial cargaba todo el archivo y fallaba con `std::bad_alloc` en equipos (o WSL)
con poca RAM. La versión final lee bloques de 32 MiB en dos pasadas; lo único que crece con el
archivo es la tabla de boletas para detectar duplicados (~48 bytes por fila). Si no cabe en
el 50 % de la memoria disponible, el programa la divide en K pasadas (memoria ≈ 1/K).

---

## 3. API simulada con latencia artificial (experimento de apoyo)

**No es un resultado de la API real.** Sirve para mostrar el efecto de paralelizar
operaciones limitadas por latencia de red. Se usó `tests/mock_api.py --latencia-ms 20`
(cada petición tarda ≥ 20 ms en el servidor local), con 200.000 filas sintéticas →
**3.288 pares `(país, fecha)` únicos = 3.288 peticiones HTTP** (una por par, gracias a la caché).

| Hilos (p) | Tiempo total [s] | `t_tasas` [s] | Speedup T1/Tp | Eficiencia S/p |
|---:|---:|---:|---:|---:|
| 1 | 69.14 | 68.92 | 1.000 | 1.000 |
| 2 | 34.54 | 34.39 | 2.002 | 1.001 |
| 4 | 17.41 | 17.23 | 3.971 | 0.993 |
| 8 | 8.78 | 8.60 | 7.873 | 0.984 |

Los cuatro resúmenes (y los archivos de errores y de tasas) resultaron **idénticos byte a byte**.

Observaciones:
- La fase de tasas es *I/O-bound*: un hilo pasa casi todo el tiempo esperando la
  respuesta, por lo que con 8 hilos se solapan 8 esperas aunque la máquina tenga
  solo 2 núcleos. Por eso aquí el speedup es casi lineal y supera al de la fase de CPU.
- Sin deduplicar, el programa haría 187.981 peticiones (una por venta válida) en vez
  de 3.288: la caché por `(país, fecha)` reduce las llamadas ~57 veces.

### Por qué la API real puede escalar menos

- **Límites del servidor**: rate limiting (HTTP 429), número máximo de conexiones
  simultáneas o un backend que atiende las consultas en serie.
- **Ancho de banda / latencia variable** de la red universitaria o doméstica.
- **Errores transitorios** (5xx/timeouts): los reintentos con backoff agregan esperas.
- **Partes secuenciales** (Amdahl): autenticación inicial, lectura del archivo,
  fusión de mapas y escritura.
- Con muchos hilos, el servidor puede responder más lento a cada petición: el
  speedup puede estancarse o incluso empeorar (eficiencia decreciente).

Recomendación para el informe: correr el benchmark real al menos dos veces y en
el mismo horario, reportar la mediana, y comentar la columna `t_tasas` por separado
del tiempo total.
