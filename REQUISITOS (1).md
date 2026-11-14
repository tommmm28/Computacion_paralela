# Requisitos para compilar y ejecutar

## Software

| Requisito | Versión | Para qué | Paquete (Ubuntu / Debian / WSL) |
|---|---|---|---|
| Compilador C++ con OpenMP | GCC ≥ 9 con C++17 (probado con 13.3, **14.2** y 15.2) | compilar `main.cpp` con `-fopenmp` | `g++` (y opcional `g++-14`) |
| libcurl con headers de desarrollo | ≥ 7.66 (probado con 8.5) | peticiones HTTPS a la API | `libcurl4-openssl-dev` |
| make | cualquiera | compilar con `make` | `make` |
| Python 3 | ≥ 3.8, **sin paquetes extra** (solo biblioteca estándar) | pruebas (`make test`) y generador de datos | `python3` |
| git | cualquiera | clonar el repositorio | `git` |
| curl o wget, gzip | cualquiera | descargar y descomprimir `ventas.csv.gz` | `curl`, `gzip` |
| Bash, awk, coreutils | los del sistema | `scripts/benchmark.sh`, `tests/validate.sh` | incluidos en Ubuntu |

## Bibliotecas que usa el programa

| Biblioteca | Tipo | Se instala con | Uso en el proyecto |
|---|---|---|---|
| **libcurl** (`<curl/curl.h>`, enlazada con `-lcurl`) | externa | `libcurl4-openssl-dev` | POST de autenticación y GET de tasas por HTTPS (`src/api.hpp`) |
| **OpenMP** (`<omp.h>`, runtime `libgomp`) | viene con GCC | `g++` (flag `-fopenmp`) | regiones paralelas, `schedule`, reducciones, `omp_get_wtime` |
| **Biblioteca estándar de C++17** | viene con GCC | `g++` | `<string>`, `<vector>`, `<unordered_map>`, `<mutex>`, `<atomic>`, `<thread>`, `<charconv>`, `<filesystem>`, `<fstream>`, etc. |
| JSON | **propia**, sin dependencias | — | `src/json_min.hpp` (no se usa nlohmann/json) |

Pruebas en Python: solo biblioteca estándar (`http.server`, `json`, `csv`, `subprocess`,
`threading`, `uuid`, `decimal`, …). **No hay que instalar nada con pip.**

### Instalación en un solo comando (Ubuntu / Debian / WSL)

```sh
sudo apt update
sudo apt install -y g++ g++-14 make libcurl4-openssl-dev python3 git curl gzip
```

### Windows

Usar **WSL** (Ubuntu dentro de Windows). En PowerShell como administrador:

```powershell
wsl --install -d Ubuntu
```

Luego abrir Ubuntu y ejecutar la instalación de arriba. El programa no fue probado de forma
nativa en Windows (MSYS2/MinGW).

### Verificar la instalación

```sh
g++ --version                                  # GCC 9 o superior
echo '#include <curl/curl.h>' | g++ -x c++ -fsyntax-only - && echo "libcurl OK"
echo 'int main(){}' | g++ -fopenmp -x c++ - -o /dev/null && echo "OpenMP OK"
python3 --version
```

## Hardware y entorno

| Recurso | Recomendado | Comentario |
|---|---|---|
| CPU | 4 núcleos o más | el benchmark mide 1, 2, 4 y 8 hilos |
| RAM | 4 GB libres o más | el programa lee por bloques; si la memoria no alcanza, detecta duplicados en varias pasadas (más lento, mismo resultado) |
| Disco | ~20 GB libres | `ventas.csv.gz` + `ventas.csv` (14 GB) + resumen y archivo de errores (~1–2 GB) |
| Red | acceso HTTPS a `api.sebastian.cl` | solo para la ejecución completa; las pruebas no usan internet |

En WSL, la memoria disponible es por defecto el 50 % de la RAM del PC. Para aumentarla, crear
`C:\Users\<usuario>\.wslconfig` con:

```
[wsl2]
memory=12GB
swap=16GB
```

y ejecutar `wsl --shutdown` en PowerShell.

## Datos de entrada (`ventas.csv`)

El archivo de ventas **no está en el repositorio** (pesa ~14 GB descomprimido; GitHub no admite
archivos de más de 100 MB). Se descarga desde el servidor del curso, que es accesible desde la
red de la UTEM (o su VPN):

```sh
cd Computacion_paralela
curl -O https://10.9.4.5:8080/ventas.csv.gz     # o: wget https://10.9.4.5:8080/ventas.csv.gz
gunzip ventas.csv.gz                            # genera ventas.csv (~14 GB)
```

Si `curl` reclama por el certificado del servidor interno, se puede descargar con el navegador
desde la red de la UTEM y descomprimirlo con `gunzip`. Formato observado: 119.115.486 líneas,
con encabezado y separador `;`.

## Credenciales

Variables de entorno (nunca en el código ni en el repositorio):

```sh
export CPYD_EMAIL='tu_correo@utem.cl'
export CPYD_APIKEY='12.345.678-5'
```

## Compilar, probar y ejecutar

```sh
make CXX=g++-14                     # o simplemente: make
make CXX=g++-14 test                # pruebas locales, sin red ni credenciales
./programa --input ventas.csv --output resumen_ventas_usd.csv --threads 8 --fallback-days 5
```
