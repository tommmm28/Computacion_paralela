// unit_tests.cpp — Pruebas unitarias SIN red (no requieren credenciales).
//
//   make unit        (o)   g++ -O2 -std=c++17 -fopenmp tests/unit_tests.cpp -o tests/unit_tests
//
// Las "tasas" que aparecen aquí son valores de PRUEBA para verificar la
// aritmética de la agregación; el programa real solo usa tasas de la API.
#include <omp.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "../src/cache_tasas.hpp"
#include "../src/json_min.hpp"
#include "../src/procesamiento.hpp"
#include "../src/validacion.hpp"

using namespace ventas;

static int fallos = 0, total = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++total;                                                                      \
        if (!(cond)) {                                                                \
            ++fallos;                                                                 \
            std::cerr << "  FALLA " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        }                                                                             \
    } while (0)

static const std::string U1 = "550e8400-e29b-41d4-a716-446655440000";
static const std::string U2 = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
static const std::string U3 = "f47ac10b-58cc-4372-a567-0e02b2c3d479";

static void test_uuid() {
    Uuid u, v;
    CHECK(parsear_uuid(U1, u));
    CHECK(uuid_a_texto(u) == U1);
    CHECK(parsear_uuid("550E8400-E29B-41D4-A716-446655440000", v));
    CHECK(u == v);  // mayúsculas = minúsculas
    CHECK(parsear_uuid("00000000-0000-0000-0000-000000000000", v));
    CHECK(!parsear_uuid("550e8400e29b41d4a716446655440000", v));        // sin guiones
    CHECK(!parsear_uuid("550e8400-e29b-41d4-a716-44665544000", v));     // corto
    CHECK(!parsear_uuid("550e8400-e29b-41d4-a716-4466554400000", v));   // largo
    CHECK(!parsear_uuid("550e8400-e29b-41d4-a716-44665544000g", v));    // no hex
    CHECK(!parsear_uuid("550e8400-e29b-41d4_a716-446655440000", v));    // separador
    CHECK(!parsear_uuid("", v));
    Uuid a, b;
    parsear_uuid("00000000-0000-0000-0000-00000000000a", a);
    parsear_uuid("00000000-0000-0000-0000-0000000000a0", b);
    CHECK(a < b);
    CHECK((uuid_a_texto(a) < uuid_a_texto(b)) == (a < b));
}

static void test_fechas() {
    int32_t d = 0;
    CHECK(parsear_fecha_hora("2025-06-20 13:45:00", d) == ResultadoFecha::OK);
    CHECK(fecha_iso(d) == "2025-06-20");
    CHECK(parsear_fecha_hora("2025-06-20T13:45:00", d) == ResultadoFecha::OK);
    CHECK(parsear_fecha_hora("2025-06-20 13:45:00.123", d) == ResultadoFecha::OK);
    CHECK(parsear_fecha_hora("2024-02-29 00:00:00", d) == ResultadoFecha::OK);       // bisiesto
    CHECK(parsear_fecha_hora("2023-02-29 00:00:00", d) == ResultadoFecha::CALENDARIO);
    CHECK(parsear_fecha_hora("2025-04-31 00:00:00", d) == ResultadoFecha::CALENDARIO);
    CHECK(parsear_fecha_hora("2025-13-01 00:00:00", d) == ResultadoFecha::CALENDARIO);
    CHECK(parsear_fecha_hora("2025-00-10 00:00:00", d) == ResultadoFecha::CALENDARIO);
    CHECK(parsear_fecha_hora("2025-06-20 24:00:00", d) == ResultadoFecha::CALENDARIO);
    CHECK(parsear_fecha_hora("2025-06-20 23:60:00", d) == ResultadoFecha::CALENDARIO);
    CHECK(parsear_fecha_hora("2025-06-20", d) == ResultadoFecha::FORMATO);
    CHECK(parsear_fecha_hora("20/06/2025 13:45:00", d) == ResultadoFecha::FORMATO);
    CHECK(parsear_fecha_hora("2025/06/20 13:45:00", d) == ResultadoFecha::FORMATO);
    CHECK(parsear_fecha_hora("2025-06-20 13:45", d) == ResultadoFecha::FORMATO);
    CHECK(parsear_fecha_hora("2025-06-20 13:45:00.", d) == ResultadoFecha::FORMATO);
    CHECK(parsear_fecha_hora("2025-06-20 13:45:00Z", d) == ResultadoFecha::FORMATO);
    CHECK(parsear_fecha_hora("2025-6-20 13:45:00", d) == ResultadoFecha::FORMATO);
    // Límites del rango (inclusive)
    CHECK(fecha_iso(DIA_INICIO) == "2023-01-01");
    CHECK(fecha_iso(DIA_FIN) == "2025-12-31");
    CHECK(DIA_FIN - DIA_INICIO + 1 == 1096);  // 2024 es bisiesto
    // Ida y vuelta de fechas
    for (int32_t x = DIA_INICIO - 400; x <= DIA_FIN + 400; ++x) {
        int32_t y = 0;
        CHECK(parsear_fecha_hora(fecha_iso(x) + " 00:00:00", y) == ResultadoFecha::OK && y == x);
    }
}

static void test_montos() {
    double v = 0;
    CHECK(parsear_monto("94058.00", v) == ResultadoMonto::OK && v == 94058.0);
    CHECK(parsear_monto("125", v) == ResultadoMonto::OK && v == 125.0);
    CHECK(parsear_monto("0.01", v) == ResultadoMonto::OK);
    CHECK(parsear_monto(".5", v) == ResultadoMonto::OK && v == 0.5);
    CHECK(parsear_monto("+3.5", v) == ResultadoMonto::OK && v == 3.5);
    CHECK(parsear_monto("0", v) == ResultadoMonto::NO_POSITIVO);
    CHECK(parsear_monto("0.00", v) == ResultadoMonto::NO_POSITIVO);
    CHECK(parsear_monto("-1", v) == ResultadoMonto::NO_POSITIVO);
    CHECK(parsear_monto("-0.5", v) == ResultadoMonto::NO_POSITIVO);
    CHECK(parsear_monto("", v) == ResultadoMonto::NO_NUMERICO);
    CHECK(parsear_monto("abc", v) == ResultadoMonto::NO_NUMERICO);
    CHECK(parsear_monto("NaN", v) == ResultadoMonto::NO_NUMERICO);
    CHECK(parsear_monto("inf", v) == ResultadoMonto::NO_NUMERICO);
    CHECK(parsear_monto("1e5", v) == ResultadoMonto::NO_NUMERICO);
    CHECK(parsear_monto("0x10", v) == ResultadoMonto::NO_NUMERICO);
    CHECK(parsear_monto("1.2.3", v) == ResultadoMonto::NO_NUMERICO);
    CHECK(parsear_monto("1,5", v) == ResultadoMonto::NO_NUMERICO);
    CHECK(parsear_monto(".", v) == ResultadoMonto::NO_NUMERICO);
    CHECK(parsear_monto("-", v) == ResultadoMonto::NO_NUMERICO);
    CHECK(parsear_monto(std::string(400, '9'), v) == ResultadoMonto::NO_NUMERICO);  // desborda double
}

static void test_filas() {
    BufferFila c;
    const std::string ok = "1," + U1 + "," + U2 + ",2024-01-15 10:00:00,1000.5";
    FilaParseada f = parsear_fila(ok, ',', c);
    CHECK(f.motivo == Motivo::OK && f.venta.pais == 1 && f.venta.monto == 1000.5 && f.boleta_valida);
    CHECK(parsear_fila(" 2 , " + U1 + " , " + U2 + " , 2024-01-15T10:00:00 , 7 ", ',', c).motivo == Motivo::OK);
    CHECK(parsear_fila("1;" + U1 + ";" + U2 + ";2024-01-15 10:00:00;1", ';', c).motivo == Motivo::OK);
    CHECK(parsear_fila("\"1\",\"" + U1 + "\",\"" + U2 + "\",\"2024-01-15 10:00:00\",\"1\"", ',', c).motivo == Motivo::OK);
    CHECK(parsear_fila("3," + U1 + "," + U2 + ",2024-01-15 10:00:00,1", ',', c).motivo == Motivo::PAIS);
    CHECK(parsear_fila("01," + U1 + "," + U2 + ",2024-01-15 10:00:00,1", ',', c).motivo == Motivo::PAIS);
    CHECK(parsear_fila("x," + U1 + "," + U2 + ",2024-01-15 10:00:00,1", ',', c).motivo == Motivo::PAIS);
    CHECK(parsear_fila("1,abc," + U2 + ",2024-01-15 10:00:00,1", ',', c).motivo == Motivo::UUID_CLIENTE);
    FilaParseada g = parsear_fila("1," + U1 + ",abc,2024-01-15 10:00:00,1", ',', c);
    CHECK(g.motivo == Motivo::UUID_BOLETA && !g.boleta_valida);
    CHECK(parsear_fila("1," + U1 + "," + U2 + ",2022-12-31 23:59:59,1", ',', c).motivo == Motivo::FECHA_RANGO);
    CHECK(parsear_fila("1," + U1 + "," + U2 + ",2026-01-01 00:00:00,1", ',', c).motivo == Motivo::FECHA_RANGO);
    CHECK(parsear_fila("1," + U1 + "," + U2 + ",2023-01-01 00:00:00,1", ',', c).motivo == Motivo::OK);
    CHECK(parsear_fila("1," + U1 + "," + U2 + ",2025-12-31 23:59:59,1", ',', c).motivo == Motivo::OK);
    CHECK(parsear_fila("1," + U1 + "," + U2 + ",2024-01-15 10:00:00,-5", ',', c).motivo == Motivo::MONTO_NO_POSITIVO);
    CHECK(parsear_fila("1," + U1 + "," + U2 + ",2024-01-15 10:00:00,abc", ',', c).motivo == Motivo::MONTO_NO_NUMERICO);
    CHECK(parsear_fila("1," + U1 + "," + U2 + ",2024-01-15 10:00:00", ',', c).motivo == Motivo::NUM_CAMPOS);
    CHECK(parsear_fila("1," + U1 + "," + U2 + ",2024-01-15 10:00:00,1,2", ',', c).motivo == Motivo::NUM_CAMPOS);
    CHECK(parsear_fila("1,\"" + U1 + "," + U2 + ",2024-01-15 10:00:00,1", ',', c).motivo == Motivo::CSV_MALFORMADO);
    // Primer error en orden de columnas, pero la boleta se extrae igual
    FilaParseada h = parsear_fila("9,abc," + U2 + ",x,-1", ',', c);
    CHECK(h.motivo == Motivo::PAIS && h.boleta_valida);
    CHECK(detectar_delimitador("1;" + U1 + ";" + U2 + ";2024-01-15 10:00:00;1") == ';');
    CHECK(detectar_delimitador("1\t" + U1 + "\t" + U2 + "\t2024-01-15 10:00:00\t1") == '\t');
    CHECK(detectar_delimitador("1," + U1 + "," + U2 + ",2024-01-15 10:00:00,1") == ',');
    CHECK(parece_encabezado("pais,id_cliente,id_boleta,fecha,monto", ','));
    CHECK(!parece_encabezado("1," + U1 + "," + U2 + ",2024-01-15 10:00:00,1", ','));
    CHECK(!parece_encabezado("x," + U1 + "," + U2 + ",2024-01-15 10:00:00,1", ','));
}

static std::string escribir_tmp(const std::string& contenido) {
    static int n = 0;
    const std::string ruta = "/tmp/cpyd_unit_" + std::to_string(::getpid()) + "_" + std::to_string(n++) + ".csv";
    std::ofstream(ruta, std::ios::binary) << contenido;
    return ruta;
}

// Ejecuta pasada 1 + pasada 2 (sin tasas) y devuelve el archivo de errores.
static std::string validar_archivo(const std::string& ruta, int hilos, size_t bloque, ResultadoPase1& p1) {
    CacheTasas cache(DIA_INICIO, DIA_FIN);
    p1 = pase1(ruta, 0, bloque, cache, hilos);
    std::ostringstream err;
    pase2(ruta, p1.delimitador, bloque, p1, cache, nullptr, hilos, err);
    return err.str();
}

static void test_duplicados_y_lectura() {
    std::string csv = "\xEF\xBB\xBFpais,cliente,boleta,fecha,monto\r\n# comentario\r\n\r\n";
    const int N = 20000;
    char b[40];
    for (int i = 0; i < N; ++i) {
        std::snprintf(b, sizeof b, "00000000-0000-4000-8000-%012d", (i % 7 == 0) ? i / 7 : i + 1000000);
        csv += std::to_string(i % 3) + "," + U1 + "," + b + ",2024-03-0" + std::to_string(1 + i % 9) + " 10:00:00,1.5\r\n";
    }
    for (int i = 0; i < 100; ++i) {  // repite boletas de filas anteriores (las i con i % 7 != 0)
        std::snprintf(b, sizeof b, "00000000-0000-4000-8000-%012d", i + 1000000);
        csv += "0," + U1 + "," + b + ",2024-03-01 10:00:00,1\n";
    }
    csv += "1," + U1 + ",00000000-0000-4000-8000-999999999999,2024-03-01 10:00:00,7";  // sin \n final
    const std::string ruta = escribir_tmp(csv);
    size_t esperado_dup = 0;
    for (int i = 0; i < 100; ++i)
        if (i % 7 != 0) ++esperado_dup;

    std::string referencia;
    // Bloques de 1 byte, 7 bytes, 4 KiB y 32 MiB; 1..8 hilos: mismo resultado.
    for (size_t bloque : {size_t(1), size_t(7), size_t(4096), size_t(32) << 20}) {
        for (int h : {1, 3, 8}) {
            ResultadoPase1 p1;
            const std::string err = validar_archivo(ruta, h, bloque, p1);
            CHECK(p1.encabezado_omitido);
            CHECK(p1.lineas_datos == static_cast<uint64_t>(N + 101));
            CHECK(p1.lineas_totales == static_cast<uint64_t>(N + 104));
            CHECK(p1.lineas_ignoradas == 2);
            CHECK(p1.conteo[static_cast<size_t>(Motivo::BOLETA_DUPLICADA)] == esperado_dup);
            CHECK(p1.duplicadas.size() == esperado_dup);
            CHECK(p1.ventas_validas == static_cast<uint64_t>(N + 101) - esperado_dup);
            // las duplicadas siempre son las apariciones posteriores, y están ordenadas
            bool orden = true;
            for (size_t k = 0; k < p1.duplicadas.size(); ++k) {
                if (p1.duplicadas[k].indice < static_cast<uint64_t>(N)) orden = false;
                if (k && p1.duplicadas[k].indice <= p1.duplicadas[k - 1].indice) orden = false;
            }
            CHECK(orden);
            if (referencia.empty()) referencia = err;
            CHECK(err == referencia);
        }
    }
    CHECK(referencia.find("primera aparición en línea 5") != std::string::npos);  // boleta de la fila i=1

    // Duplicados en K pasadas (archivos grandes) == una sola pasada
    {
        CacheTasas cache(DIA_INICIO, DIA_FIN);
        const ResultadoPase1 base = pase1(ruta, 0, 4096, cache, 4);
        for (unsigned K : {1u, 2u, 8u, 64u}) {
            double tl = 0, tc = 0;
            const auto dups = detectar_duplicados_por_pasadas(ruta, 0, 1000, 3, K, tl, tc);
            CHECK(dups.size() == esperado_dup);
            const ResultadoPase1 p = pase1(ruta, 0, 4096, cache, 4, &dups);
            CHECK(p.conteo == base.conteo && p.ventas_validas == base.ventas_validas && p.marcas == base.marcas);
            bool iguales = p.duplicadas.size() == base.duplicadas.size();
            for (size_t k = 0; iguales && k < p.duplicadas.size(); ++k)
                iguales = p.duplicadas[k].indice == base.duplicadas[k].indice &&
                          p.duplicadas[k].linea_primera == base.duplicadas[k].linea_primera;
            CHECK(iguales);
        }
        CHECK(estimar_filas(ruta) > 15000 && estimar_filas(ruta) < 25000);
    }
    std::remove(ruta.c_str());

    // Tabla de boletas: crecimiento y consultas
    TablaBoletas t(4);
    for (uint32_t i = 1; i <= 100000; ++i) CHECK(t.insertar(Uuid{i, i * 7ULL}, i) == i);
    bool ok = true;
    for (uint32_t i = 1; i <= 100000; ++i) ok = ok && t.insertar(Uuid{i, i * 7ULL}, 999999) == i;
    CHECK(ok && t.size() == 100000);
}

static void test_json() {
    json::Valor v;
    std::string e;
    CHECK(json::parsear(R"({"jwt":"abc.def.ghi"})", v, e) && v.miembro("jwt") && v.miembro("jwt")->cadena == "abc.def.ghi");
    CHECK(json::parsear(R"( {"country":"Chile","date":"2025-06-20","rate":940.58} )", v, e));
    CHECK(v.miembro("rate")->tipo == json::Tipo::NUMERO && v.miembro("rate")->numero == 940.58);
    CHECK(json::parsear(R"({"rate":4.1e3})", v, e) && v.miembro("rate")->numero == 4100.0);
    CHECK(json::parsear(R"({"rate":null})", v, e) && v.miembro("rate")->tipo == json::Tipo::NULO);
    CHECK(json::parsear(R"({"data":{"jwt":"x"}})", v, e) && v.buscar("jwt") && v.buscar("jwt")->cadena == "x");
    CHECK(json::parsear(R"({"s":"a\"b\\cé😀","a":[1,2,{"b":false}],"t":true})", v, e));
    CHECK(v.miembro("s")->cadena == "a\"b\\c\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(!json::parsear(R"({"rate":940.58)", v, e));        // truncado
    CHECK(!json::parsear(R"({"rate":01})", v, e));           // cero inicial
    CHECK(!json::parsear(R"({"rate":1.})", v, e));
    CHECK(!json::parsear(R"({rate:1})", v, e));
    CHECK(!json::parsear(R"({"a":1} x)", v, e));
    CHECK(!json::parsear("<html>502 Bad Gateway</html>", v, e));
    CHECK(!json::parsear("", v, e));
    CHECK(!json::parsear(std::string(200, '[') + std::string(200, ']'), v, e));  // anidamiento excesivo
    CHECK(json::escapar("a\"b\\c\n") == "a\\\"b\\\\c\\n");
}

static void test_cache_concurrente() {
    // 64 hilos piden repetidamente 30 claves: cada clave se carga EXACTAMENTE una vez.
    CacheTasas cache(DIA_INICIO, DIA_INICIO + 9);
    std::atomic<int> cargas{0};
#pragma omp parallel num_threads(16)
    {
#pragma omp for schedule(dynamic, 1)
        for (int k = 0; k < 3000; ++k) {
            const size_t idx = static_cast<size_t>(k % 30);
            const EntradaTasa& e = cache.obtener(idx, [&](EntradaTasa& x) {
                cargas.fetch_add(1);
                x.estado = EstadoTasa::OK;
                x.tasa = 100.0 + static_cast<double>(idx);  // valor de prueba
            });
            if (e.estado != EstadoTasa::OK || e.tasa != 100.0 + static_cast<double>(idx)) {
#pragma omp critical
                ++fallos;
            }
        }
    }
    CHECK(cargas.load() == 30);
    CHECK(cache.cargas() == 30);
    CHECK(cache.solicitudes() == 3000);
    CHECK(cache.pais_de(cache.indice(2, DIA_INICIO + 5)) == 2);
    CHECK(cache.dia_de(cache.indice(2, DIA_INICIO + 5)) == DIA_INICIO + 5);
}

static void test_pipeline_sin_red() {
    // Pipeline completo con un "consultor" falso: Chile 2024-01-01 no tiene tasa,
    // Colombia falla siempre; Ecuador tiene tasa. Valores solo para la prueba.
    const std::string csv =
        "0," + U1 + ",00000000-0000-4000-8000-000000000001,2024-01-02 10:00:00,1000\n"
        "0," + U1 + ",00000000-0000-4000-8000-000000000002,2024-01-02 11:00:00,500\n"
        "0," + U2 + ",00000000-0000-4000-8000-000000000003,2024-01-01 11:00:00,500\n"   // sin tasa
        "1," + U1 + ",00000000-0000-4000-8000-000000000004,2024-01-02 11:00:00,4000\n"  // error API
        "2," + U1 + ",00000000-0000-4000-8000-000000000005,2024-01-02 11:00:00,10\n"
        "2," + U3 + ",00000000-0000-4000-8000-000000000006,2024-01-03 11:00:00,30\n"
        "2," + U3 + ",00000000-0000-4000-8000-000000000006,2024-01-03 12:00:00,99\n";  // duplicada
    const std::string ruta = escribir_tmp(csv);
    auto consultor = [](int& /*ctx*/, int pais, int32_t dia, EntradaTasa& e) {
        if (pais == 1) { e.estado = EstadoTasa::ERROR; e.detalle = "HTTP 500"; return; }
        if (pais == 0 && fecha_iso(dia) == "2024-01-01") { e.estado = EstadoTasa::SIN_TASA; e.detalle = "HTTP 404"; return; }
        e.estado = EstadoTasa::OK;
        e.tasa = (pais == 0) ? 1000.0 : 1.0;
    };
    std::vector<std::string> salidas;
    for (int fallback : {0, 3}) {
        for (int h : {1, 2, 4, 8}) {
            for (size_t bloque : {size_t(16), size_t(1) << 20}) {
                CacheTasas cache(DIA_INICIO - fallback, DIA_FIN);
                ResultadoPase1 p1 = pase1(ruta, 0, bloque, cache, h);
                auto claves = claves_desde_marcas(p1.marcas);
                CHECK(claves.size() == 5);
                EstadisticasTasas est;
                auto res = resolver_tasas(cache, claves, fallback, h, [] { return 0; }, consultor, est);
                std::ostringstream err;
                ResultadoPase2 p2 = pase2(ruta, p1.delimitador, bloque, p1, cache, &res, h, err);
                const auto& filas = p2.filas;
                CHECK(p2.incidencias == (fallback == 0 ? 3u : 2u));  // duplicada + tasas
                if (fallback == 0) {
                    CHECK(p2.omitidas_por_tasa == 2 && p2.convertidas == 4);
                    CHECK(filas.size() == 3);
                    CHECK(filas[0].pais == 0 && std::fabs(static_cast<double>(filas[0].total_usd) - 1.5) < 1e-12);
                    CHECK(filas[1].pais == 2 && std::fabs(static_cast<double>(filas[1].total_usd) - 10.0) < 1e-12);
                    CHECK(filas[2].pais == 2 && std::fabs(static_cast<double>(filas[2].total_usd) - 30.0) < 1e-12);
                    CHECK(est.sin_tasa == 1 && est.errores == 1 && est.ok_directas == 3);
                    CHECK(err.str().find("\n3,sin tasa disponible") != std::string::npos);
                    CHECK(err.str().find("\n4,error de API") != std::string::npos);
                    CHECK(err.str().find("\n7,boleta duplicada: primera aparición en línea 6") != std::string::npos);
                } else {
                    CHECK(p2.omitidas_por_tasa == 1);  // solo el error de API (Chile usa 2023-12-31)
                    CHECK(filas.size() == 4);
                    CHECK(est.ok_fallback == 1);
                }
                std::string s;
                for (const auto& f : filas) {
                    char num[64];
                    std::snprintf(num, sizeof num, "%.2Lf", f.total_usd);
                    s += std::to_string(f.pais) + uuid_a_texto(f.cliente) + num + ";";
                }
                salidas.push_back(s + err.str());
            }
        }
    }
    for (size_t i = 1; i < 8; ++i) CHECK(salidas[i] == salidas[0]);   // independiente de hilos y bloque
    for (size_t i = 9; i < 16; ++i) CHECK(salidas[i] == salidas[8]);
    std::remove(ruta.c_str());
}

int main() {
    test_uuid();
    test_fechas();
    test_montos();
    test_filas();
    test_duplicados_y_lectura();
    test_json();
    test_cache_concurrente();
    test_pipeline_sin_red();
    std::cout << "Pruebas unitarias: " << (total - fallos) << "/" << total << " OK\n";
    return fallos == 0 ? 0 : 1;
}
