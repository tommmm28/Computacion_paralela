// main.cpp — Resumen de ventas en USD por (país, cliente) con OpenMP + API CPYD.
//
// Compilación (equivalente a `make`):
//   g++ -O2 -std=c++17 -fopenmp main.cpp -lcurl -o programa
//
// Uso:
//   export CPYD_EMAIL='tu_correo@utem.cl'  CPYD_APIKEY='12.345.678-5'
//   ./programa --input ventas.csv --output resumen_ventas_usd.csv --threads 8
//
// Pipeline (detalle en README.md). El archivo se lee por bloques en dos
// pasadas, así la memoria no depende del tamaño del archivo:
//   1. Pasada 1: validación por fila en paralelo, boletas duplicadas en paralelo
//      por fragmentos y marcas de pares (país, fecha) con reducción OR.
//   2. Autenticación JWT (una vez).
//   3. Consulta paralela de tasas únicas (schedule(dynamic)) con caché sincronizada.
//   4. Pasada 2: conversión y suma en paralelo con mapas privados por hilo;
//      incidencias escritas en orden de línea.
//   5. Fusión secuencial, orden y escritura.
#include <omp.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/cache_tasas.hpp"
#include "src/procesamiento.hpp"
#include "src/validacion.hpp"

#if __has_include(<curl/curl.h>)
#include "src/api.hpp"
#define HAY_CURL 1
#else
#define HAY_CURL 0
#endif

namespace {

using namespace ventas;

struct Opciones {
    std::string input = "ventas.csv";
    std::string output = "resumen_ventas_usd.csv";
    std::string metricas;           // CSV opcional para el benchmark
    int hilos = 0;                  // 0 = omp_get_max_threads()
    int dias_fallback = 0;          // 0 = sin fallback (política por defecto)
    int reintentos = 3;
    long timeout_s = 30;
    char delimitador = 0;           // 0 = automático
    bool solo_validar = false;
    bool pais_como_nombre = false;
    size_t bytes_bloque = 32u << 20;  // tamaño de bloque de lectura (32 MiB)
    int mem_mb = 0;                   // memoria para detectar duplicados (0 = automática)
};

void ayuda() {
    std::cout <<
        "Uso: programa --input ventas.csv --output resumen_ventas_usd.csv --threads N [opciones]\n\n"
        "  --input ARCHIVO          CSV de ventas (por defecto: ventas.csv)\n"
        "  --output ARCHIVO         CSV de resumen (por defecto: resumen_ventas_usd.csv)\n"
        "                           También genera <output>.errors.csv y <output>.tasas.csv\n"
        "  --threads N              hilos OpenMP (por defecto: los disponibles)\n"
        "  --validate-only          solo valida (no usa la API ni credenciales)\n"
        "  --fallback-days N        si una fecha no tiene tasa, usar la última tasa\n"
        "                           anterior consultando hasta N días hacia atrás\n"
        "                           (por defecto 0: la venta se registra y se omite)\n"
        "  --retries N              reintentos ante errores transitorios (por defecto 3)\n"
        "  --timeout S              timeout por petición HTTP en segundos (por defecto 30)\n"
        "  --delimiter D            ',', ';', 'tab' o 'auto' (por defecto auto)\n"
        "  --country-format F       'code' (0,1,2; por defecto) o 'name' (Chile, ...)\n"
        "  --metrics ARCHIVO        agrega una fila con tiempos por fase (benchmark)\n"
        "  --block-mb N             tamaño del bloque de lectura en MiB (por defecto 32);\n"
        "                           el archivo nunca se carga completo en memoria\n"
        "  --mem-mb N               memoria (MiB) para detectar boletas duplicadas; si no\n"
        "                           alcanza, se usan varias pasadas (por defecto: 50 % de\n"
        "                           la memoria disponible)\n"
        "  --help                   muestra esta ayuda\n\n"
        "Variables de entorno: CPYD_EMAIL, CPYD_APIKEY (obligatorias salvo --validate-only)\n"
        "                      CPYD_API_BASE (opcional, solo para pruebas con API simulada)\n";
}

int entero(const std::string& opcion, const std::string& v, int min, int max) {
    size_t n = 0;
    int x = 0;
    try {
        x = std::stoi(v, &n);
    } catch (...) {
        n = 0;
    }
    if (n != v.size() || x < min || x > max)
        throw std::runtime_error(opcion + " debe ser un entero entre " + std::to_string(min) + " y " + std::to_string(max));
    return x;
}

Opciones leer_argumentos(int argc, char** argv) {
    Opciones o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i], v;
        bool tiene_valor = false;
        if (a.rfind("--", 0) == 0 && a.find('=') != std::string::npos) {  // --opcion=valor
            v = a.substr(a.find('=') + 1);
            a = a.substr(0, a.find('='));
            tiene_valor = true;
        }
        auto valor = [&]() -> std::string {
            if (tiene_valor) return v;
            if (i + 1 >= argc) throw std::runtime_error("falta el valor de " + a);
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { ayuda(); std::exit(0); }
        else if (a == "--input") o.input = valor();
        else if (a == "--output") o.output = valor();
        else if (a == "--threads") o.hilos = entero(a, valor(), 1, 1024);
        else if (a == "--validate-only") o.solo_validar = true;
        else if (a == "--fallback-days") o.dias_fallback = entero(a, valor(), 0, 60);
        else if (a == "--retries") o.reintentos = entero(a, valor(), 0, 10);
        else if (a == "--timeout") o.timeout_s = entero(a, valor(), 1, 600);
        else if (a == "--metrics") o.metricas = valor();
        else if (a == "--mem-mb") o.mem_mb = entero(a, valor(), 64, 1 << 20);
        else if (a == "--block-mb") o.bytes_bloque = static_cast<size_t>(entero(a, valor(), 1, 1024)) << 20;
        else if (a == "--country-format") {
            const std::string f = valor();
            if (f == "code") o.pais_como_nombre = false;
            else if (f == "name") o.pais_como_nombre = true;
            else throw std::runtime_error("--country-format debe ser 'code' o 'name'");
        } else if (a == "--delimiter") {
            const std::string d = valor();
            if (d == "auto") o.delimitador = 0;
            else if (d == "tab" || d == "\\t") o.delimitador = '\t';
            else if (d == "," || d == ";") o.delimitador = d[0];
            else throw std::runtime_error("--delimiter debe ser ',', ';', 'tab' o 'auto'");
        } else {
            throw std::runtime_error("argumento desconocido: " + a + " (use --help)");
        }
    }
    if (o.hilos == 0) o.hilos = omp_get_max_threads();
    if (o.input.empty() || o.output.empty()) throw std::runtime_error("--input y --output no pueden ser vacíos");
    return o;
}

struct Tiempos {
    double lectura = 0, validacion = 0, tasas = 0, agregacion = 0, escritura = 0, total = 0;
};

void escribir_metricas(const Opciones& o, const char* modo, const Tiempos& t, size_t lineas, size_t validas,
                       size_t claves, long llamadas, size_t clientes) {
    if (o.metricas.empty()) return;
    bool nuevo = false;
    {
        std::ifstream prueba(o.metricas);
        nuevo = !prueba.good() || prueba.peek() == std::ifstream::traits_type::eof();
    }
    std::ofstream m(o.metricas, std::ios::app);
    if (!m) {
        std::cerr << "Advertencia: no se pudo escribir métricas en " << o.metricas << "\n";
        return;
    }
    if (nuevo)
        m << "modo,hilos,lineas,ventas_validas,claves_unicas,llamadas_http,clientes,"
             "t_lectura,t_validacion,t_tasas,t_agregacion,t_escritura,t_total\n";
    m << modo << ',' << o.hilos << ',' << lineas << ',' << validas << ',' << claves << ',' << llamadas << ','
      << clientes << std::fixed << std::setprecision(6) << ',' << t.lectura << ',' << t.validacion << ','
      << t.tasas << ',' << t.agregacion << ',' << t.escritura << ',' << t.total << '\n';
}

void imprimir_validacion(const ResultadoPase1& p) {
    std::cout << "Líneas leídas: " << p.lineas_totales << " (datos: " << p.lineas_datos
              << ", vacías/comentarios: " << p.lineas_ignoradas
              << (p.encabezado_omitido ? ", encabezado omitido: 1" : "") << ")\n";
    std::cout << "Delimitador: " << (p.delimitador == '\t' ? std::string("tab") : std::string(1, p.delimitador)) << "\n";
    std::cout << "Ventas válidas: " << p.ventas_validas << "; filas omitidas en validación: "
              << (p.lineas_datos - p.ventas_validas) << "\n";
    for (size_t m = 1; m < NUM_MOTIVOS; ++m)
        if (p.conteo[m]) std::cout << "  - " << texto_motivo(static_cast<Motivo>(m)) << ": " << p.conteo[m] << "\n";
}

const char* variable_entorno(const char* nombre) {
    const char* v = std::getenv(nombre);
    return (v && *v) ? v : nullptr;
}

std::ofstream abrir_errores(const std::string& ruta) {
    std::ofstream f(ruta, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("no se pudo escribir el archivo de errores: " + ruta);
    return f;
}

int ejecutar(int argc, char** argv) {
    const Opciones o = leer_argumentos(argc, argv);
    omp_set_dynamic(0);
    omp_set_num_threads(o.hilos);
    const std::string ruta_errores = o.output + ".errors.csv";
    const std::string ruta_tasas = o.output + ".tasas.csv";
    Tiempos t;
    const double t0 = omp_get_wtime();

    // 0) ¿Cabe la tabla de boletas en memoria? Si no, se detectan los duplicados
    //    en K pasadas (cada una guarda 1/K de las boletas).
    const uint64_t filas_estimadas = estimar_filas(o.input);
    const double disponible = memoria_disponible_mb();
    const double presupuesto = o.mem_mb > 0 ? o.mem_mb : (disponible > 0 ? disponible * 0.5 : 2048.0);
    const double necesario = static_cast<double>(filas_estimadas) * 48.0 / (1024.0 * 1024.0);  // ~48 B por boleta
    unsigned pasadas_dup = 1;
    while (pasadas_dup < NUM_FRAGMENTOS && necesario / pasadas_dup > presupuesto) pasadas_dup *= 2;
    std::cout << "Filas estimadas: " << filas_estimadas << "; tabla de boletas ~" << static_cast<long>(necesario)
              << " MiB; memoria para duplicados: " << static_cast<long>(presupuesto) << " MiB → "
              << pasadas_dup << (pasadas_dup == 1 ? " pasada\n" : " pasadas de detección de duplicados\n") << std::flush;

    // 1-3) Pasada 1: lectura por bloques + validación + duplicados + pares (país, fecha)
    CacheTasas cache(DIA_INICIO - o.dias_fallback, DIA_FIN);
    std::vector<Duplicado> dups;
    double t_lect_dup = 0, t_calc_dup = 0;
    if (pasadas_dup > 1)
        dups = detectar_duplicados_por_pasadas(o.input, o.delimitador, o.bytes_bloque, o.hilos, pasadas_dup,
                                               t_lect_dup, t_calc_dup);
    const ResultadoPase1 p1 = pase1(o.input, o.delimitador, o.bytes_bloque, cache, o.hilos,
                                    pasadas_dup > 1 ? &dups : nullptr);
    std::vector<Duplicado>().swap(dups);
    t.lectura = p1.t_lectura + t_lect_dup;
    t.validacion = p1.t_calculo + t_calc_dup;
    const std::vector<size_t> claves = claves_desde_marcas(p1.marcas);
    std::cout << "Hilos OpenMP: " << o.hilos << "\n";
    imprimir_validacion(p1);
    std::cout << "Pares (país, fecha) únicos a consultar: " << claves.size() << "\n";

    if (o.solo_validar || p1.ventas_validas == 0) {
        std::ofstream err = abrir_errores(ruta_errores);
        const ResultadoPase2 p2 = pase2(o.input, p1.delimitador, o.bytes_bloque, p1, cache, nullptr, o.hilos, err);
        t.lectura += p2.t_lectura;
        t.agregacion = p2.t_calculo;
        t.total = omp_get_wtime() - t0;
        if (o.solo_validar) {
            std::cout << "Modo --validate-only: no se consultó la API ni se generó " << o.output << ".\n"
                      << "Incidencias en " << ruta_errores << "\n";
            std::cout << std::fixed << std::setprecision(4) << "Tiempo: lectura " << t.lectura << " s, validación "
                      << t.validacion << " s, total " << t.total << " s\n";
            escribir_metricas(o, "validacion", t, p1.lineas_datos, p1.ventas_validas, claves.size(), 0, 0);
        } else {
            escribir_resumen(o.output, {}, o.pais_como_nombre);
            std::cout << "No hay ventas válidas: se escribió un resumen vacío en " << o.output << "\n";
        }
        return 0;
    }

#if !HAY_CURL
    throw std::runtime_error("compilado sin libcurl: instale libcurl4-openssl-dev o use --validate-only");
#else
    // 4) Credenciales (solo desde variables de entorno) y autenticación única
    const char* email = variable_entorno("CPYD_EMAIL");
    const char* clave = variable_entorno("CPYD_APIKEY");
    if (!email || !clave)
        throw std::runtime_error("defina las variables de entorno CPYD_EMAIL y CPYD_APIKEY (ver README)");
    ConfigApi cfg;
    if (const char* base = variable_entorno("CPYD_API_BASE")) {
        cfg.base_url = base;
        while (!cfg.base_url.empty() && cfg.base_url.back() == '/') cfg.base_url.pop_back();
        if (cfg.base_url != URL_API_POR_DEFECTO)
            std::cerr << "Aviso: usando CPYD_API_BASE=" << cfg.base_url << " (no es la API oficial)\n";
    }
    cfg.reintentos = o.reintentos;
    cfg.timeout_s = o.timeout_s;
    cfg.connect_timeout_s = std::min(10L, o.timeout_s);

    const double t2 = omp_get_wtime();
    CurlGlobal curl_global;  // antes de crear hilos
    GestorToken token(email, clave, cfg);
    {
        ClienteHttp http(cfg);
        token.autenticar_inicial(http);  // lanza si falla → error global
    }
    std::cout << "Autenticación correcta (JWT obtenido; no se muestra).\n";
    ApiCambio api(cfg, token);

    // 5) Tasas en paralelo
    EstadisticasTasas est;
    const std::vector<Resolucion> res = resolver_tasas(
        cache, claves, o.dias_fallback, o.hilos,
        [&cfg]() { return std::make_unique<ClienteHttp>(cfg); },
        [&api](std::unique_ptr<ClienteHttp>& http, int pais, int32_t dia, EntradaTasa& e) {
            api.consultar(pais, dia, *http, e);
        },
        est);
    const double t3 = omp_get_wtime();
    t.tasas = t3 - t2;
    std::cout << "Tasas: " << est.ok_directas << " obtenidas, " << est.ok_fallback << " por fallback, "
              << est.sin_tasa << " sin tasa, " << est.errores << " con error de API. Peticiones HTTP: "
              << api.llamadas() << " (reintentos: " << api.reintentos() << ", refrescos de token: "
              << token.refrescos() << ")\n";

    // 6) Pasada 2: conversión y agregación paralela + incidencias en orden
    std::ofstream err = abrir_errores(ruta_errores);
    const ResultadoPase2 p2 = pase2(o.input, p1.delimitador, o.bytes_bloque, p1, cache, &res, o.hilos, err);
    err.close();
    t.lectura += p2.t_lectura;
    t.agregacion = p2.t_calculo;

    // 7) Escritura del resumen y de la bitácora de tasas
    const double t4 = omp_get_wtime();
    escribir_resumen(o.output, p2.filas, o.pais_como_nombre);
    escribir_tasas(ruta_tasas, cache, claves, res);
    const double t5 = omp_get_wtime();
    t.escritura = t5 - t4;
    t.total = t5 - t0;

    std::cout << "Ventas convertidas: " << p2.convertidas << "; omitidas por tasa: " << p2.omitidas_por_tasa
              << "; clientes (país, id): " << p2.filas.size() << "\n";
    std::cout << "Resumen: " << o.output << "\nIncidencias: " << ruta_errores << " (" << p2.incidencias
              << ")\nTasas usadas: " << ruta_tasas << "\n";
    std::cout << std::fixed << std::setprecision(4) << "Tiempo [s]: lectura " << t.lectura << ", validación "
              << t.validacion << ", tasas " << t.tasas << ", agregación " << t.agregacion << ", escritura "
              << t.escritura << ", total " << t.total << "\n";
    escribir_metricas(o, "completo", t, p1.lineas_datos, p1.ventas_validas, claves.size(), api.llamadas(), p2.filas.size());

    if (p2.convertidas == 0) {
        std::cerr << "Error: no se pudo convertir ninguna venta (revise " << ruta_errores << ")\n";
        return 3;
    }
    return 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return ejecutar(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
