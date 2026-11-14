// procesamiento.hpp — Lectura por bloques, validación paralela, duplicados,
// resolución de tasas y agregación paralela por (país, cliente).
//
// Flujo en dos pasadas sobre el archivo (memoria acotada):
//   pasada 1: validar + duplicados + marcar pares (país, fecha)  → pase1()
//   tasas:    consulta paralela de los pares únicos               → resolver_tasas()
//   pasada 2: convertir + sumar por hilo + escribir incidencias   → pase2()
//
// Este módulo no depende de libcurl: la consulta de tasas se inyecta como
// función, de modo que todo el pipeline se puede probar sin red.
#pragma once

#include <omp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cache_tasas.hpp"
#include "validacion.hpp"

namespace ventas {

// ---------------------------------------------------------------------------
// Detección de formato
// ---------------------------------------------------------------------------
// Cuenta delimitadores fuera de comillas y elige el que produce 5 campos.
inline char detectar_delimitador(std::string_view linea) {
    const char candidatos[3] = {',', ';', '\t'};
    size_t cuenta[3] = {0, 0, 0};
    bool comillas = false;
    for (char c : linea) {
        if (c == '"') comillas = !comillas;
        if (comillas) continue;
        for (int k = 0; k < 3; ++k)
            if (c == candidatos[k]) ++cuenta[k];
    }
    for (int k = 0; k < 3; ++k)
        if (cuenta[k] == 4) return candidatos[k];
    int mejor = 0;
    for (int k = 1; k < 3; ++k)
        if (cuenta[k] > cuenta[mejor]) mejor = k;
    return cuenta[mejor] > 0 ? candidatos[mejor] : ',';
}

// Una primera línea cuyo primer campo contiene letras (p. ej. "pais") y que no
// tiene UUIDs se interpreta como encabezado y se omite.
inline bool parece_encabezado(std::string_view linea, char delim) {
    BufferFila b;
    if (!dividir_csv(linea, delim, b) || b.campos.size() < 2) return false;
    const auto& c = b.campos;
    const std::string_view p = recortar(c[0]);
    const bool tiene_letra = std::any_of(p.begin(), p.end(), [](char ch) { return std::isalpha(static_cast<unsigned char>(ch)) != 0; });
    Uuid u;
    const bool hay_uuid = parsear_uuid(recortar(c[1]), u) || (c.size() > 2 && parsear_uuid(recortar(c[2]), u));
    return tiene_letra && !hay_uuid;
}

// ---------------------------------------------------------------------------
// Lectura por bloques (memoria acotada)
// ---------------------------------------------------------------------------
// El archivo NO se carga completo en memoria: se lee en bloques de tamaño fijo
// (32 MiB por defecto) cortados en un fin de línea. Cada bloque se procesa en
// paralelo y se descarta. Así el programa funciona con archivos de varios GB
// aunque el equipo (o WSL) tenga poca RAM.

struct Bloque {
    std::string datos;                       // bytes del bloque
    std::vector<std::string_view> lineas;    // líneas de datos (vistas sobre `datos`)
    std::vector<uint64_t> numero;            // número de línea física (1-based) de cada una
    uint64_t indice_inicial = 0;             // índice global (entre líneas de datos) de lineas[0]
};

class LectorCsv {
public:
    // delimitador_forzado = 0 → detección automática con la primera línea de datos.
    LectorCsv(const std::string& ruta, char delimitador_forzado, size_t bytes_bloque)
        : in_(ruta, std::ios::binary),
          bytes_bloque_(std::max<size_t>(bytes_bloque, 1)),
          delimitador_(delimitador_forzado ? delimitador_forzado : ','),
          forzado_(delimitador_forzado != 0) {
        if (!in_) throw std::runtime_error("no se pudo abrir el archivo de entrada: " + ruta);
    }

    // Llena `b` con las siguientes líneas. Devuelve false cuando ya no queda nada.
    // (Un bloque puede venir sin líneas de datos si solo tenía vacías/comentarios.)
    bool leer(Bloque& b) {
        b.lineas.clear();
        b.numero.clear();
        b.indice_inicial = lineas_datos_;
        b.datos.swap(resto_);
        resto_.clear();
        size_t corte = std::string::npos;
        for (;;) {
            if (!eof_) {
                const size_t previo = b.datos.size();
                b.datos.resize(previo + bytes_bloque_);
                in_.read(&b.datos[previo], static_cast<std::streamsize>(bytes_bloque_));
                const size_t leidos = static_cast<size_t>(in_.gcount());
                b.datos.resize(previo + leidos);
                if (in_.bad()) throw std::runtime_error("error de lectura del archivo de entrada");
                if (leidos < bytes_bloque_) eof_ = true;
                if (inicio_) {  // BOM UTF-8
                    inicio_ = false;
                    if (b.datos.size() >= 3 && static_cast<unsigned char>(b.datos[0]) == 0xEF &&
                        static_cast<unsigned char>(b.datos[1]) == 0xBB && static_cast<unsigned char>(b.datos[2]) == 0xBF)
                        b.datos.erase(0, 3);
                }
            }
            if (eof_) {
                corte = b.datos.size();
                break;
            }
            const size_t p = b.datos.rfind('\n');
            if (p != std::string::npos) {
                corte = p + 1;
                break;
            }
            // una línea más larga que el bloque: seguir leyendo
        }
        if (b.datos.empty()) return false;
        resto_.assign(b.datos, corte, std::string::npos);
        b.datos.resize(corte);
        separar_lineas(b);
        return true;
    }

    char delimitador() const { return delimitador_; }
    bool encabezado_omitido() const { return encabezado_; }
    uint64_t lineas_totales() const { return num_; }
    uint64_t lineas_ignoradas() const { return ignoradas_; }
    uint64_t lineas_datos() const { return lineas_datos_; }

private:
    void separar_lineas(Bloque& b) {
        const std::string_view todo(b.datos);
        size_t pos = 0;
        while (pos < todo.size()) {
            size_t fin = todo.find('\n', pos);
            if (fin == std::string_view::npos) fin = todo.size();
            std::string_view linea = todo.substr(pos, fin - pos);
            pos = fin + 1;
            ++num_;
            if (!linea.empty() && linea.back() == '\r') linea.remove_suffix(1);
            const std::string_view t = recortar(linea);
            if (t.empty() || t[0] == '#') {
                ++ignoradas_;
                continue;
            }
            if (primera_de_datos_) {
                primera_de_datos_ = false;
                if (!forzado_) delimitador_ = detectar_delimitador(linea);
                if (parece_encabezado(linea, delimitador_)) {
                    encabezado_ = true;
                    continue;
                }
            }
            b.lineas.push_back(linea);
            b.numero.push_back(num_);
            ++lineas_datos_;
        }
    }

    std::ifstream in_;
    size_t bytes_bloque_;
    std::string resto_;   // línea incompleta al final del bloque anterior
    bool eof_ = false, inicio_ = true, primera_de_datos_ = true, encabezado_ = false;
    char delimitador_;
    bool forzado_;
    uint64_t num_ = 0, ignoradas_ = 0, lineas_datos_ = 0;
};

// ---------------------------------------------------------------------------
// Boletas duplicadas: tablas hash por fragmento
// ---------------------------------------------------------------------------
constexpr unsigned NUM_FRAGMENTOS = 64;  // fragmentos del espacio de boletas
constexpr uint8_t SIN_FRAGMENTO = 0xFF;

inline uint8_t fragmento_de(const Uuid& u) {
    return static_cast<uint8_t>(mezclar64(u.lo + 0x9e3779b97f4a7c15ULL * u.hi) >> 58);  // 0..63
}

// Tabla hash de direccionamiento abierto (sondeo lineal) boleta → línea de su
// primera aparición. No asigna memoria por elemento (a diferencia de
// std::unordered_map) y crece al duplicarse cuando supera 75 % de ocupación.
class TablaBoletas {
public:
    static constexpr uint32_t VACIO = 0;  // las líneas se numeran desde 1

    explicit TablaBoletas(size_t capacidad = 1024) { reservar(capacidad); }

    // Inserta (k, linea) si k no existe. Devuelve la línea ya asociada a k,
    // o `linea` si se insertó.
    uint32_t insertar(const Uuid& k, uint32_t linea) {
        if ((n_ + 1) * 4 > valores_.size() * 3) crecer();
        size_t h = HashUuid{}(k) & mascara_;
        for (;;) {
            if (valores_[h] == VACIO) {
                claves_[h] = k;
                valores_[h] = linea;
                ++n_;
                return linea;
            }
            if (claves_[h] == k) return valores_[h];
            h = (h + 1) & mascara_;
        }
    }
    size_t size() const { return n_; }

private:
    void reservar(size_t cap) {
        size_t c = 16;
        while (c < cap) c <<= 1;
        claves_.assign(c, Uuid{});
        valores_.assign(c, VACIO);
        mascara_ = c - 1;
        n_ = 0;
    }
    void crecer() {
        std::vector<Uuid> viejas_claves;
        std::vector<uint32_t> viejos_valores;
        viejas_claves.swap(claves_);
        viejos_valores.swap(valores_);
        reservar(viejos_valores.size() * 2);
        for (size_t i = 0; i < viejos_valores.size(); ++i)
            if (viejos_valores[i] != VACIO) insertar(viejas_claves[i], viejos_valores[i]);
    }

    std::vector<Uuid> claves_;
    std::vector<uint32_t> valores_;
    size_t mascara_ = 0, n_ = 0;
};

// ---------------------------------------------------------------------------
// Pasada 1: validar, detectar duplicados y marcar los pares (país, fecha)
// ---------------------------------------------------------------------------
struct Duplicado {
    uint64_t indice;        // índice global de la línea de datos duplicada
    uint32_t linea_primera; // línea física de la primera aparición
};

struct ResultadoPase1 {
    std::array<size_t, NUM_MOTIVOS> conteo{};  // motivos de validación (incluye duplicadas)
    std::vector<Duplicado> duplicadas;          // ordenadas por índice
    std::vector<unsigned char> marcas;          // marcas[k] = 1 si la clave k de la caché se necesita
    uint64_t lineas_totales = 0, lineas_ignoradas = 0, lineas_datos = 0, ventas_validas = 0;
    char delimitador = ',';
    bool encabezado_omitido = false;
    double t_lectura = 0, t_calculo = 0;
};

// dups_precalculados != nullptr → los duplicados ya se detectaron con
// detectar_duplicados_por_pasadas() (archivos muy grandes) y aquí solo se usan.
inline ResultadoPase1 pase1(const std::string& ruta, char delimitador_forzado, size_t bytes_bloque,
                            const CacheTasas& cache, int hilos,
                            const std::vector<Duplicado>* dups_precalculados = nullptr) {
    LectorCsv lector(ruta, delimitador_forzado, bytes_bloque);
    ResultadoPase1 r;
    size_t cursor_dup = 0;
    const size_t N = cache.tamano();
    r.marcas.assign(N, 0);
    std::vector<TablaBoletas> tablas(dups_precalculados ? 0 : NUM_FRAGMENTOS);  // persisten entre bloques
    Bloque b;
    std::vector<FilaParseada> filas;
    std::vector<uint8_t> fragmento;
    std::vector<uint32_t> primera;
    std::vector<std::vector<Duplicado>> dup_hilo;

    for (;;) {
        double t0 = omp_get_wtime();
        if (!lector.leer(b)) break;
        r.t_lectura += omp_get_wtime() - t0;
        t0 = omp_get_wtime();
        const size_t n = b.lineas.size();
        const long long nn = static_cast<long long>(n);
        if (n && b.numero.back() > UINT32_MAX - 1) throw std::runtime_error("archivo con demasiadas líneas");
        filas.resize(n);
        fragmento.assign(n, SIN_FRAGMENTO);
        primera.assign(n, TablaBoletas::VACIO);

        // (1) Parseo y validación por fila: iteraciones independientes, cada una
        //     escribe solo filas[i] y fragmento[i]. Costo uniforme → static.
#pragma omp parallel num_threads(hilos)
        {
            BufferFila buf;  // buffers privados de cada hilo
            buf.campos.reserve(8);
#pragma omp for schedule(static)
            for (long long i = 0; i < nn; ++i) {
                const size_t k = static_cast<size_t>(i);
                filas[k] = parsear_fila(b.lineas[k], lector.delimitador(), buf);
                if (filas[k].boleta_valida) fragmento[k] = fragmento_de(filas[k].venta.boleta);
            }
        }

        // (2) Duplicados por fragmentos: el hilo t es dueño exclusivo de las
        //     tablas f con f % T == t. Cada boleta cae en un único fragmento, así
        //     que un solo hilo ve todas sus apariciones y en orden de archivo
        //     (los bloques se procesan en orden) → gana la primera aparición,
        //     sin locks y con resultado determinista.
        if (dups_precalculados) {
            const auto& d = *dups_precalculados;
            while (cursor_dup < d.size() && d[cursor_dup].indice < b.indice_inicial + n) {
                primera[d[cursor_dup].indice - b.indice_inicial] = d[cursor_dup].linea_primera;
                ++cursor_dup;
            }
        } else {
#pragma omp parallel num_threads(hilos)
            {
                const unsigned T = static_cast<unsigned>(omp_get_num_threads());
                const unsigned t = static_cast<unsigned>(omp_get_thread_num());
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t f = fragmento[i];
                    if (f == SIN_FRAGMENTO || f % T != t) continue;
                    const uint32_t linea = static_cast<uint32_t>(b.numero[i]);
                    const uint32_t previa = tablas[f].insertar(filas[i].venta.boleta, linea);
                    if (previa != linea) primera[i] = previa;  // solo este hilo escribe primera[i]
                }
            }
        }

        // (3) Clasificación final, conteos y marcas de (país, fecha) con
        //     reducciones sobre arreglos; duplicadas en listas por hilo sobre
        //     tramos contiguos (se concatenan en orden → quedan ordenadas).
        size_t* conteo = r.conteo.data();
        unsigned char* marcas = r.marcas.data();
        uint64_t validas = 0;
#pragma omp parallel num_threads(hilos) reduction(+ : conteo[:NUM_MOTIVOS], validas) reduction(| : marcas[:N])
        {
            const size_t T = static_cast<size_t>(omp_get_num_threads());
            const size_t t = static_cast<size_t>(omp_get_thread_num());
#pragma omp single
            dup_hilo.assign(T, {});
            const size_t ini = n * t / T, fin = n * (t + 1) / T;
            for (size_t i = ini; i < fin; ++i) {
                Motivo m = filas[i].motivo;
                if (m == Motivo::OK && primera[i] != TablaBoletas::VACIO) {
                    m = Motivo::BOLETA_DUPLICADA;
                    dup_hilo[t].push_back({b.indice_inicial + i, primera[i]});
                }
                ++conteo[static_cast<size_t>(m)];
                if (m == Motivo::OK) {
                    ++validas;
                    marcas[cache.indice(filas[i].venta.pais, filas[i].venta.dia)] = 1;
                }
            }
        }
        r.ventas_validas += validas;
        for (auto& v : dup_hilo) r.duplicadas.insert(r.duplicadas.end(), v.begin(), v.end());
        r.t_calculo += omp_get_wtime() - t0;
    }
    r.lineas_totales = lector.lineas_totales();
    r.lineas_ignoradas = lector.lineas_ignoradas();
    r.lineas_datos = lector.lineas_datos();
    r.delimitador = lector.delimitador();
    r.encabezado_omitido = lector.encabezado_omitido();
    return r;
}

// ---------------------------------------------------------------------------
// Duplicados en varias pasadas (archivos más grandes que la memoria)
// ---------------------------------------------------------------------------
// La tabla de boletas ocupa ~40 bytes por fila. Si no cabe en la memoria
// disponible, el espacio de boletas (64 fragmentos) se divide en K grupos y el
// archivo se recorre K veces; en cada recorrido solo se guardan las boletas de
// un grupo, así la memoria pico es ~1/K. El resultado es idéntico al de una
// sola pasada (cada boleta pertenece a un único fragmento).

// Misma condición que parsear_fila para que una boleta "cuente": CSV bien
// formado, 5 campos y UUID válido en la 3.ª columna.
inline bool extraer_boleta(std::string_view linea, char delim, BufferFila& buf, Uuid& boleta) {
    return dividir_csv(linea, delim, buf) && buf.campos.size() == 5 && parsear_uuid(recortar(buf.campos[2]), boleta);
}

inline std::vector<Duplicado> detectar_duplicados_por_pasadas(const std::string& ruta, char delimitador_forzado,
                                                              size_t bytes_bloque, int hilos, unsigned pasadas,
                                                              double& t_lectura, double& t_calculo) {
    std::vector<Duplicado> dups;
    const unsigned por_pasada = NUM_FRAGMENTOS / pasadas;
    Bloque b;
    std::vector<Uuid> boletas;
    std::vector<uint8_t> fragmento;
    std::vector<uint32_t> primera;
    for (unsigned j = 0; j < pasadas; ++j) {
        const unsigned f_ini = j * por_pasada, f_fin = f_ini + por_pasada;
        std::vector<TablaBoletas> tablas(NUM_FRAGMENTOS, TablaBoletas(16));
        LectorCsv lector(ruta, delimitador_forzado, bytes_bloque);
        for (;;) {
            double t0 = omp_get_wtime();
            if (!lector.leer(b)) break;
            t_lectura += omp_get_wtime() - t0;
            t0 = omp_get_wtime();
            const size_t n = b.lineas.size();
            const long long nn = static_cast<long long>(n);
            boletas.resize(n);
            fragmento.assign(n, SIN_FRAGMENTO);
            primera.assign(n, TablaBoletas::VACIO);
#pragma omp parallel num_threads(hilos)
            {
                BufferFila buf;
#pragma omp for schedule(static)
                for (long long i = 0; i < nn; ++i) {
                    const size_t k = static_cast<size_t>(i);
                    if (extraer_boleta(b.lineas[k], lector.delimitador(), buf, boletas[k])) {
                        const uint8_t f = fragmento_de(boletas[k]);
                        if (f >= f_ini && f < f_fin) fragmento[k] = f;
                    }
                }
            }
#pragma omp parallel num_threads(hilos)
            {
                const unsigned T = static_cast<unsigned>(omp_get_num_threads());
                const unsigned t = static_cast<unsigned>(omp_get_thread_num());
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t f = fragmento[i];
                    if (f == SIN_FRAGMENTO || f % T != t) continue;
                    const uint32_t linea = static_cast<uint32_t>(b.numero[i]);
                    const uint32_t previa = tablas[f].insertar(boletas[i], linea);
                    if (previa != linea) primera[i] = previa;
                }
            }
            for (size_t i = 0; i < n; ++i)
                if (primera[i] != TablaBoletas::VACIO) dups.push_back({b.indice_inicial + i, primera[i]});
            t_calculo += omp_get_wtime() - t0;
        }
    }
    std::sort(dups.begin(), dups.end(), [](const Duplicado& a, const Duplicado& c) { return a.indice < c.indice; });
    return dups;
}

// Estima la cantidad de filas a partir del tamaño del archivo y del largo
// promedio de las líneas del primer MiB.
inline uint64_t estimar_filas(const std::string& ruta) {
    std::error_code ec;
    const uint64_t tam = std::filesystem::file_size(ruta, ec);
    if (ec || tam == 0) return 0;
    std::ifstream f(ruta, std::ios::binary);
    std::string muestra(1 << 20, '\0');
    f.read(&muestra[0], static_cast<std::streamsize>(muestra.size()));
    muestra.resize(static_cast<size_t>(f.gcount()));
    const size_t lineas = static_cast<size_t>(std::count(muestra.begin(), muestra.end(), '\n'));
    if (lineas == 0) return 1;
    return static_cast<uint64_t>(static_cast<double>(tam) / (static_cast<double>(muestra.size()) / static_cast<double>(lineas)));
}

// Memoria disponible según /proc/meminfo (Linux/WSL), en MiB; 0 si no se sabe.
inline double memoria_disponible_mb() {
    std::ifstream f("/proc/meminfo");
    std::string clave;
    double kb = 0;
    while (f >> clave) {
        if (clave == "MemAvailable:") {
            f >> kb;
            return kb / 1024.0;
        }
        std::getline(f, clave);
    }
    return 0;
}

inline std::vector<size_t> claves_desde_marcas(const std::vector<unsigned char>& marcas) {
    std::vector<size_t> claves;
    for (size_t k = 0; k < marcas.size(); ++k)
        if (marcas[k]) claves.push_back(k);
    return claves;
}

// ---------------------------------------------------------------------------
// Resolución de tasas: consulta paralela de claves únicas + fallback opcional
// ---------------------------------------------------------------------------
struct Resolucion {
    EstadoTasa estado = EstadoTasa::PENDIENTE;
    double tasa = 0.0;
    int32_t dia_tasa = 0;  // fecha cuya tasa se usó (== día de la venta salvo fallback)
    Motivo motivo = Motivo::OK;
    std::string detalle;
};

struct EstadisticasTasas {
    size_t claves = 0, ok_directas = 0, ok_fallback = 0, sin_tasa = 0, errores = 0;
};

// crear(): crea el contexto privado de cada hilo (p. ej. un handle HTTP).
// consultar(ctx, pais, dia, EntradaTasa&): llena la entrada; no debe lanzar.
template <class Crear, class Consultar>
std::vector<Resolucion> resolver_tasas(CacheTasas& cache, const std::vector<size_t>& claves, int dias_fallback,
                                       int hilos, Crear crear, Consultar consultar, EstadisticasTasas& est) {
    std::vector<Resolucion> res(cache.tamano());
    const long long nk = static_cast<long long>(claves.size());

#pragma omp parallel num_threads(hilos)
    {
        auto ctx = crear();  // contexto privado del hilo (conexión reutilizable)
        auto cargar = [&](size_t idx) {
            return [&, idx](EntradaTasa& e) { consultar(ctx, cache.pais_de(idx), cache.dia_de(idx), e); };
        };

        // Fase A: una consulta por clave única. La latencia de red varía mucho
        // entre peticiones → schedule(dynamic, 1) para balancear la carga.
#pragma omp for schedule(dynamic, 1)
        for (long long k = 0; k < nk; ++k) {
            const size_t idx = claves[static_cast<size_t>(k)];
            cache.obtener(idx, cargar(idx));
        }
        // Barrera implícita: todas las claves directas ya están cargadas.

        // Fase B (opcional): para claves sin tasa, buscar la última tasa
        // anterior consultando la API día por día hacia atrás. Varias claves
        // pueden necesitar la misma fecha anterior: la caché (call_once)
        // garantiza que se consulta una sola vez.
        if (dias_fallback > 0) {
#pragma omp for schedule(dynamic, 1)
            for (long long k = 0; k < nk; ++k) {
                const size_t idx = claves[static_cast<size_t>(k)];
                if (cache.entrada(idx).estado != EstadoTasa::SIN_TASA) continue;
                const int pais = cache.pais_de(idx);
                const int32_t dia = cache.dia_de(idx);
                for (int r = 1; r <= dias_fallback; ++r) {
                    if (!cache.contiene(pais, dia - r)) break;
                    const size_t idx2 = cache.indice(pais, dia - r);
                    const EntradaTasa& e2 = cache.obtener(idx2, cargar(idx2));
                    if (e2.estado == EstadoTasa::OK) {
                        Resolucion& z = res[idx];  // solo esta iteración escribe res[idx]
                        z.estado = EstadoTasa::OK;
                        z.tasa = e2.tasa;
                        z.dia_tasa = dia - r;
                        z.detalle = "fallback: se usó la tasa de " + fecha_iso(dia - r);
                        break;
                    }
                    if (e2.estado == EstadoTasa::ERROR) break;  // no insistir si la API falla
                }
            }
        }
    }

    // Consolidación secuencial (≈3.300 claves como máximo).
    est = EstadisticasTasas{};
    est.claves = claves.size();
    for (size_t idx : claves) {
        Resolucion& z = res[idx];
        const EntradaTasa& e = cache.entrada(idx);
        const std::string clave = std::string(pais_api(cache.pais_de(idx))) + " " + fecha_iso(cache.dia_de(idx));
        if (z.estado == EstadoTasa::OK) {  // resuelta por fallback
            ++est.ok_fallback;
            continue;
        }
        if (e.estado == EstadoTasa::OK) {
            z.estado = EstadoTasa::OK;
            z.tasa = e.tasa;
            z.dia_tasa = cache.dia_de(idx);
            ++est.ok_directas;
        } else if (e.estado == EstadoTasa::SIN_TASA) {
            z.estado = EstadoTasa::SIN_TASA;
            z.motivo = Motivo::SIN_TASA;
            z.detalle = clave + ": " + e.detalle;
            if (dias_fallback > 0)
                z.detalle += "; tampoco hubo tasa en los " + std::to_string(dias_fallback) + " días anteriores";
            ++est.sin_tasa;
        } else {
            z.estado = EstadoTasa::ERROR;
            z.motivo = Motivo::ERROR_API;
            z.detalle = clave + ": " + e.detalle;
            ++est.errores;
        }
    }
    return res;
}

// ---------------------------------------------------------------------------
// Acumuladores privados por hilo
// ---------------------------------------------------------------------------
struct ClaveCliente {
    Uuid cliente;
    uint8_t pais;
    bool operator==(const ClaveCliente& o) const { return pais == o.pais && cliente == o.cliente; }
};
struct HashClaveCliente {
    size_t operator()(const ClaveCliente& k) const noexcept {
        return HashUuid{}(k.cliente) ^ (static_cast<size_t>(k.pais) * 0x9e3779b97f4a7c15ULL);
    }
};
struct Acumulado {
    long double total_usd = 0.0L;  // 80 bits en x86: el redondeo final no depende del nº de hilos
    size_t ventas = 0;
};
using MapaTotales = std::unordered_map<ClaveCliente, Acumulado, HashClaveCliente>;

struct FilaResumen {
    uint8_t pais;
    Uuid cliente;
    long double total_usd;
    size_t ventas;
};

// ---------------------------------------------------------------------------
// Pasada 2: convertir y sumar en paralelo + escribir incidencias en orden
// ---------------------------------------------------------------------------
// Se vuelve a leer el archivo por bloques (las tasas ya están resueltas).
// Re-parsear cuesta poco comparado con guardar todas las ventas en memoria.

struct ResultadoPase2 {
    std::vector<FilaResumen> filas;   // resumen ordenado por (país, cliente)
    uint64_t incidencias = 0;         // filas escritas en el archivo de errores
    uint64_t omitidas_por_tasa = 0;   // ventas válidas sin tasa o con error de API
    uint64_t convertidas = 0;         // ventas sumadas al resumen
    double t_lectura = 0, t_calculo = 0;
};

struct IncidenciaBloque {
    uint32_t i;       // índice dentro del bloque
    Motivo motivo;
    std::string detalle;
};

inline std::string escapar_csv(std::string_view s);

// res == nullptr → modo solo validación: no convierte, solo escribe incidencias.
inline ResultadoPase2 pase2(const std::string& ruta, char delimitador, size_t bytes_bloque, const ResultadoPase1& p1,
                            const CacheTasas& cache, const std::vector<Resolucion>* res, int hilos,
                            std::ostream& errores) {
    LectorCsv lector(ruta, delimitador, bytes_bloque);
    ResultadoPase2 r;
    // 64 mapas, uno por fragmento del id de cliente. Cada cliente cae en un solo
    // mapa y cada mapa lo modifica un solo hilo por bloque (f % T == t): no hay
    // copias por hilo (la memoria no se multiplica por el número de hilos) ni
    // fusión final, y la suma de cada cliente se hace en orden de archivo.
    std::vector<MapaTotales> mapas(NUM_FRAGMENTOS);
    std::vector<std::vector<IncidenciaBloque>> inc_hilo;
    Bloque b;
    std::vector<Venta> ventas;
    std::vector<long double> usd;
    std::vector<uint8_t> frag_cliente;
    std::vector<uint32_t> dup_primera;
    size_t cursor_dup = 0;
    errores << "linea,motivo,contenido\n";

    for (;;) {
        double t0 = omp_get_wtime();
        if (!lector.leer(b)) break;
        r.t_lectura += omp_get_wtime() - t0;
        t0 = omp_get_wtime();
        const size_t n = b.lineas.size();

        // Duplicadas de este bloque (lista ordenada de la pasada 1)
        dup_primera.assign(n, 0);
        while (cursor_dup < p1.duplicadas.size() && p1.duplicadas[cursor_dup].indice < b.indice_inicial + n) {
            dup_primera[p1.duplicadas[cursor_dup].indice - b.indice_inicial] = p1.duplicadas[cursor_dup].linea_primera;
            ++cursor_dup;
        }
        ventas.resize(n);
        usd.resize(n);
        frag_cliente.assign(n, SIN_FRAGMENTO);
        uint64_t omitidas = 0, convertidas = 0;

#pragma omp parallel num_threads(hilos) reduction(+ : omitidas, convertidas)
        {
            const size_t T = static_cast<size_t>(omp_get_num_threads());
            const size_t t = static_cast<size_t>(omp_get_thread_num());
#pragma omp single
            inc_hilo.assign(T, {});

            // (a) Parseo, clasificación y conversión de un tramo contiguo:
            //     cada hilo escribe solo ventas[i], usd[i], frag_cliente[i] de su
            //     tramo y su propia lista de incidencias (queda en orden).
            BufferFila buf;
            buf.campos.reserve(8);
            const size_t ini = n * t / T, fin = n * (t + 1) / T;
            for (size_t i = ini; i < fin; ++i) {
                const FilaParseada f = parsear_fila(b.lineas[i], lector.delimitador(), buf);
                Motivo m = f.motivo;
                std::string detalle;
                if (m == Motivo::OK && dup_primera[i] != 0) {
                    m = Motivo::BOLETA_DUPLICADA;
                    detalle = "primera aparición en línea " + std::to_string(dup_primera[i]);
                } else if (m == Motivo::OK && res) {
                    const Venta& v = f.venta;
                    const Resolucion& z = (*res)[cache.indice(v.pais, v.dia)];  // solo lectura
                    if (z.estado == EstadoTasa::OK) {
                        ventas[i] = v;
                        usd[i] = static_cast<long double>(v.monto) / static_cast<long double>(z.tasa);  // monto_USD = monto_local / rate
                        frag_cliente[i] = fragmento_de(v.cliente);
                        ++convertidas;
                    } else {
                        m = z.motivo;
                        detalle = z.detalle;
                        ++omitidas;
                    }
                }
                if (m != Motivo::OK) inc_hilo[t].push_back({static_cast<uint32_t>(i), m, std::move(detalle)});
            }
#pragma omp barrier
            // (b) Suma: el hilo t actualiza solo los mapas de los fragmentos
            //     g % T == t → ningún mapa es modificado por dos hilos.
            for (size_t i = 0; i < n; ++i) {
                const uint8_t g = frag_cliente[i];
                if (g == SIN_FRAGMENTO || g % T != t) continue;
                Acumulado& a = mapas[g][ClaveCliente{ventas[i].cliente, ventas[i].pais}];
                a.total_usd += usd[i];
                ++a.ventas;
            }
        }
        r.omitidas_por_tasa += omitidas;
        r.convertidas += convertidas;

        // Escritura secuencial de las incidencias del bloque, en orden de línea.
        for (const auto& lista : inc_hilo)
            for (const auto& inc : lista) {
                std::string motivo = texto_motivo(inc.motivo);
                if (!inc.detalle.empty()) motivo += ": " + inc.detalle;
                errores << b.numero[inc.i] << ',' << escapar_csv(motivo) << ',' << escapar_csv(b.lineas[inc.i]) << '\n';
                ++r.incidencias;
            }
        if (!errores) throw std::runtime_error("error al escribir el archivo de errores");
        r.t_calculo += omp_get_wtime() - t0;
    }
    if (lector.lineas_datos() != p1.lineas_datos)
        throw std::runtime_error("el archivo de entrada cambió durante la ejecución");

    // Resultado: los mapas son disjuntos, basta concatenarlos y ordenar.
    const double t0 = omp_get_wtime();
    size_t total = 0;
    for (const auto& m : mapas) total += m.size();
    r.filas.reserve(total);
    for (auto& m : mapas) {
        for (auto& kv : m) r.filas.push_back({kv.first.pais, kv.first.cliente, kv.second.total_usd, kv.second.ventas});
        MapaTotales().swap(m);  // liberar memoria
    }
    // Orden: país y luego id de cliente. Comparar (hi, lo) equivale a comparar
    // el texto canónico en minúsculas (largo fijo, guiones en posiciones fijas).
    std::sort(r.filas.begin(), r.filas.end(), [](const FilaResumen& a, const FilaResumen& c) {
        return a.pais != c.pais ? a.pais < c.pais : a.cliente < c.cliente;
    });
    r.t_calculo += omp_get_wtime() - t0;
    return r;
}

// ---------------------------------------------------------------------------
// Escritura de archivos
// ---------------------------------------------------------------------------
inline std::string escapar_csv(std::string_view s) {
    if (s.find_first_of(",;\"\r\n\t") == std::string_view::npos) return std::string(s);
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += '"';
        o += c;
    }
    return o + '"';
}

inline void escribir_resumen(const std::string& ruta, const std::vector<FilaResumen>& filas, bool pais_como_nombre) {
    std::ofstream out(ruta, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("no se pudo escribir el archivo de salida: " + ruta);
    out << "pais,id_cliente,total_usd\n";
    char num[64];
    for (const auto& f : filas) {
        std::snprintf(num, sizeof num, "%.2Lf", f.total_usd);
        if (pais_como_nombre)
            out << pais_nombre(f.pais);
        else
            out << static_cast<int>(f.pais);
        out << ',' << uuid_a_texto(f.cliente) << ',' << num << '\n';
    }
    out.flush();
    if (!out) throw std::runtime_error("error al escribir " + ruta);
}

// Bitácora de tasas usadas: permite auditar que todas provienen de la API.
inline void escribir_tasas(const std::string& ruta, const CacheTasas& cache, const std::vector<size_t>& claves,
                           const std::vector<Resolucion>& res) {
    std::ofstream out(ruta, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("no se pudo escribir " + ruta);
    out << "pais,fecha,estado,rate,fecha_rate,detalle\n";
    char num[64];
    for (size_t idx : claves) {
        const Resolucion& z = res[idx];
        out << pais_api(cache.pais_de(idx)) << ',' << fecha_iso(cache.dia_de(idx)) << ',' << texto_estado(z.estado) << ',';
        if (z.estado == EstadoTasa::OK) {
            std::snprintf(num, sizeof num, "%.10g", z.tasa);
            out << num << ',' << fecha_iso(z.dia_tasa);
        } else {
            out << ',';
        }
        out << ',' << escapar_csv(z.detalle) << '\n';
    }
}

}  // namespace ventas
