// validacion.hpp — Parseo y validación de una fila del CSV de ventas.
//
// Todo el módulo es puro (sin estado global ni E/S), por lo que sus funciones
// son seguras para llamarse concurrentemente desde varios hilos OpenMP.
#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ventas {

// ---------------------------------------------------------------------------
// Países
// ---------------------------------------------------------------------------
constexpr int NUM_PAISES = 3;

inline const char* pais_api(int p) {  // nombre usado en la URL de la API
    static const char* const n[NUM_PAISES] = {"chile", "colombia", "ecuador"};
    return n[p];
}
inline const char* pais_nombre(int p) {  // nombre legible
    static const char* const n[NUM_PAISES] = {"Chile", "Colombia", "Ecuador"};
    return n[p];
}

// ---------------------------------------------------------------------------
// Fechas: se representan como número de días desde 1970-01-01 (int32_t).
// Algoritmos "days_from_civil"/"civil_from_days" de Howard Hinnant.
// ---------------------------------------------------------------------------
constexpr int32_t dias_desde_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

inline void civil_desde_dias(int32_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2) ? 1 : 0;
}

inline std::string fecha_iso(int32_t dia) {  // YYYY-MM-DD
    int y;
    unsigned m, d;
    civil_desde_dias(dia, y, m, d);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
    return buf;
}

constexpr bool es_bisiesto(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
constexpr unsigned dias_del_mes(int y, unsigned m) {
    constexpr unsigned t[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return t[m - 1] + ((m == 2 && es_bisiesto(y)) ? 1u : 0u);
}

// Rango de procesamiento obligatorio (inclusive).
constexpr int32_t DIA_INICIO = dias_desde_civil(2023, 1, 1);
constexpr int32_t DIA_FIN = dias_desde_civil(2025, 12, 31);

// ---------------------------------------------------------------------------
// UUID: se guarda como 128 bits (hi, lo). Comparar/hashear enteros es mucho
// más rápido que comparar strings, y normaliza mayúsculas/minúsculas.
// ---------------------------------------------------------------------------
struct Uuid {
    uint64_t hi = 0, lo = 0;
    bool operator==(const Uuid& o) const { return hi == o.hi && lo == o.lo; }
    bool operator!=(const Uuid& o) const { return !(*this == o); }
    bool operator<(const Uuid& o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
};

inline uint64_t mezclar64(uint64_t x) {  // splitmix64 finalizer
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
struct HashUuid {
    size_t operator()(const Uuid& u) const noexcept {
        return static_cast<size_t>(mezclar64(u.hi ^ mezclar64(u.lo)));
    }
};

inline int valor_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Formato canónico 8-4-4-4-12 hexadecimal (mayúsculas o minúsculas).
// No se exige versión/variante RFC 4122 (ver supuestos en el README).
inline bool parsear_uuid(std::string_view s, Uuid& out) {
    if (s.size() != 36) return false;
    uint64_t partes[2] = {0, 0};
    int nibble = 0;
    for (size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
            continue;
        }
        const int v = valor_hex(c);
        if (v < 0) return false;
        partes[nibble / 16] = (partes[nibble / 16] << 4) | static_cast<uint64_t>(v);
        ++nibble;
    }
    out.hi = partes[0];
    out.lo = partes[1];
    return true;
}

inline std::string uuid_a_texto(const Uuid& u) {  // minúsculas canónicas
    static const char* hex = "0123456789abcdef";
    std::string s(36, '-');
    int pos = 0;
    for (int i = 0; i < 32; ++i) {
        if (pos == 8 || pos == 13 || pos == 18 || pos == 23) ++pos;
        const uint64_t w = i < 16 ? u.hi : u.lo;
        const int shift = 60 - 4 * (i % 16);
        s[static_cast<size_t>(pos++)] = hex[(w >> shift) & 0xF];
    }
    return s;
}

// ---------------------------------------------------------------------------
// Utilidades de texto / CSV
// ---------------------------------------------------------------------------
inline std::string_view recortar(std::string_view s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string_view::npos) return {};
    const size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

// Buffers reutilizables por hilo para dividir filas sin asignar memoria.
struct BufferFila {
    std::vector<std::string_view> campos;
    std::string almacen;  // solo se usa si la fila tiene comillas
};

// Divide una fila CSV respetando comillas dobles ("" = comilla escapada).
// Caso normal (sin comillas): los campos son vistas sobre la propia fila, sin
// copiar ni asignar memoria. Con comillas: el texto des-escapado se escribe en
// b.almacen, reservado al largo de la fila para que las vistas no se invaliden.
// Devuelve false si hay comillas sin cerrar.
inline bool dividir_csv(std::string_view fila, char delim, BufferFila& b) {
    b.campos.clear();
    if (fila.find('"') == std::string_view::npos) {
        size_t ini = 0;
        for (;;) {
            const size_t p = fila.find(delim, ini);
            if (p == std::string_view::npos) {
                b.campos.push_back(fila.substr(ini));
                return true;
            }
            b.campos.push_back(fila.substr(ini, p - ini));
            ini = p + 1;
        }
    }
    b.almacen.clear();
    b.almacen.reserve(fila.size());
    size_t inicio = 0;
    bool en_comillas = false;
    auto cerrar_campo = [&]() {
        b.campos.emplace_back(b.almacen.data() + inicio, b.almacen.size() - inicio);
        inicio = b.almacen.size();
    };
    for (size_t i = 0; i < fila.size(); ++i) {
        const char c = fila[i];
        if (c == '"') {
            if (en_comillas && i + 1 < fila.size() && fila[i + 1] == '"') {
                b.almacen += '"';
                ++i;
            } else {
                en_comillas = !en_comillas;
            }
        } else if (c == delim && !en_comillas) {
            cerrar_campo();
        } else {
            b.almacen += c;
        }
    }
    if (en_comillas) return false;
    cerrar_campo();
    return true;
}

// ---------------------------------------------------------------------------
// Fecha/hora: "YYYY-MM-DD HH:MM:SS" o "YYYY-MM-DDTHH:MM:SS",
// opcionalmente con fracción de segundo ".ffffff".
// ---------------------------------------------------------------------------
enum class ResultadoFecha { OK, FORMATO, CALENDARIO };

inline bool digitos(std::string_view s, size_t pos, size_t n, int& v) {
    v = 0;
    for (size_t i = pos; i < pos + n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    return true;
}

inline ResultadoFecha parsear_fecha_hora(std::string_view s, int32_t& dia) {
    if (s.size() < 19) return ResultadoFecha::FORMATO;
    int y, mo, d, h, mi, se;
    if (!digitos(s, 0, 4, y) || s[4] != '-' || !digitos(s, 5, 2, mo) || s[7] != '-' ||
        !digitos(s, 8, 2, d) || (s[10] != ' ' && s[10] != 'T') || !digitos(s, 11, 2, h) ||
        s[13] != ':' || !digitos(s, 14, 2, mi) || s[16] != ':' || !digitos(s, 17, 2, se))
        return ResultadoFecha::FORMATO;
    if (s.size() > 19) {  // fracción de segundo opcional
        if (s[19] != '.' || s.size() == 20) return ResultadoFecha::FORMATO;
        for (size_t i = 20; i < s.size(); ++i)
            if (s[i] < '0' || s[i] > '9') return ResultadoFecha::FORMATO;
    }
    if (mo < 1 || mo > 12 || d < 1 || static_cast<unsigned>(d) > dias_del_mes(y, static_cast<unsigned>(mo)) ||
        h > 23 || mi > 59 || se > 59)
        return ResultadoFecha::CALENDARIO;
    dia = dias_desde_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return ResultadoFecha::OK;
}

// ---------------------------------------------------------------------------
// Monto: decimal con punto, sin separador de miles ni notación científica.
// Se usa std::from_chars (independiente del locale y sin asignar memoria).
// ---------------------------------------------------------------------------
enum class ResultadoMonto { OK, NO_NUMERICO, NO_POSITIVO };

inline ResultadoMonto parsear_monto(std::string_view s, double& v) {
    if (s.empty()) return ResultadoMonto::NO_NUMERICO;
    bool negativo = false;
    size_t i = 0;
    if (s[0] == '+' || s[0] == '-') {
        negativo = s[0] == '-';
        i = 1;
    }
    size_t dig = 0, puntos = 0;
    for (size_t k = i; k < s.size(); ++k) {
        if (s[k] >= '0' && s[k] <= '9')
            ++dig;
        else if (s[k] == '.' && puntos == 0)
            ++puntos;
        else
            return ResultadoMonto::NO_NUMERICO;
    }
    if (dig == 0) return ResultadoMonto::NO_NUMERICO;
    const auto r = std::from_chars(s.data() + i, s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(v))
        return ResultadoMonto::NO_NUMERICO;
    if (negativo) v = -v;
    if (!(v > 0.0)) return ResultadoMonto::NO_POSITIVO;
    return ResultadoMonto::OK;
}

// ---------------------------------------------------------------------------
// Motivos de incidencia (columna "motivo" del archivo de errores)
// ---------------------------------------------------------------------------
enum class Motivo : uint8_t {
    OK = 0,
    CSV_MALFORMADO,
    NUM_CAMPOS,
    PAIS,
    UUID_CLIENTE,
    UUID_BOLETA,
    FECHA_FORMATO,
    FECHA_CALENDARIO,
    FECHA_RANGO,
    MONTO_NO_NUMERICO,
    MONTO_NO_POSITIVO,
    BOLETA_DUPLICADA,
    SIN_TASA,
    ERROR_API,
    CANTIDAD
};
constexpr size_t NUM_MOTIVOS = static_cast<size_t>(Motivo::CANTIDAD);

inline const char* texto_motivo(Motivo m) {
    switch (m) {
        case Motivo::OK: return "ok";
        case Motivo::CSV_MALFORMADO: return "CSV malformado (comillas sin cerrar)";
        case Motivo::NUM_CAMPOS: return "cantidad de campos distinta de 5";
        case Motivo::PAIS: return "país inválido (debe ser 0, 1 o 2)";
        case Motivo::UUID_CLIENTE: return "UUID de cliente inválido";
        case Motivo::UUID_BOLETA: return "UUID de boleta inválido";
        case Motivo::FECHA_FORMATO: return "fecha/hora con formato inválido";
        case Motivo::FECHA_CALENDARIO: return "fecha/hora inexistente en el calendario";
        case Motivo::FECHA_RANGO: return "fecha fuera del rango 2023-01-01..2025-12-31";
        case Motivo::MONTO_NO_NUMERICO: return "monto no numérico";
        case Motivo::MONTO_NO_POSITIVO: return "monto no positivo";
        case Motivo::BOLETA_DUPLICADA: return "boleta duplicada";
        case Motivo::SIN_TASA: return "sin tasa disponible";
        case Motivo::ERROR_API: return "error de API al obtener tasa";
        default: return "desconocido";
    }
}

// Clave corta y estable (para resúmenes y métricas).
inline const char* clave_motivo(Motivo m) {
    switch (m) {
        case Motivo::OK: return "ok";
        case Motivo::CSV_MALFORMADO: return "csv_malformado";
        case Motivo::NUM_CAMPOS: return "num_campos";
        case Motivo::PAIS: return "pais";
        case Motivo::UUID_CLIENTE: return "uuid_cliente";
        case Motivo::UUID_BOLETA: return "uuid_boleta";
        case Motivo::FECHA_FORMATO: return "fecha_formato";
        case Motivo::FECHA_CALENDARIO: return "fecha_calendario";
        case Motivo::FECHA_RANGO: return "fecha_rango";
        case Motivo::MONTO_NO_NUMERICO: return "monto_no_numerico";
        case Motivo::MONTO_NO_POSITIVO: return "monto_no_positivo";
        case Motivo::BOLETA_DUPLICADA: return "boleta_duplicada";
        case Motivo::SIN_TASA: return "sin_tasa";
        case Motivo::ERROR_API: return "error_api";
        default: return "desconocido";
    }
}

// ---------------------------------------------------------------------------
// Venta validada y resultado del parseo de una fila
// ---------------------------------------------------------------------------
struct Venta {
    Uuid cliente;
    Uuid boleta;
    double monto = 0.0;   // moneda local
    int32_t dia = 0;      // días desde 1970-01-01
    uint32_t idx_linea = 0;  // índice en el vector de líneas leídas
    uint8_t pais = 0;
};

struct FilaParseada {
    Venta venta;
    Motivo motivo = Motivo::OK;
    bool boleta_valida = false;  // la boleta es un UUID válido (participa del control de duplicados)
};

// Valida una fila completa. Reporta el primer problema encontrado, pero
// extrae la boleta aunque otro campo falle, para que el control de
// duplicados sea sobre todo el archivo.
inline FilaParseada parsear_fila(std::string_view linea, char delim, BufferFila& buf) {
    FilaParseada r;
    const auto& campos = buf.campos;
    if (!dividir_csv(linea, delim, buf)) {
        r.motivo = Motivo::CSV_MALFORMADO;
        return r;
    }
    if (campos.size() != 5) {
        r.motivo = Motivo::NUM_CAMPOS;
        return r;
    }
    auto fallar = [&r](Motivo m) {
        if (r.motivo == Motivo::OK) r.motivo = m;
    };

    // Boleta primero (solo para poder detectar duplicados), luego en orden de columnas.
    r.boleta_valida = parsear_uuid(recortar(campos[2]), r.venta.boleta);

    const std::string_view p = recortar(campos[0]);
    if (p.size() == 1 && p[0] >= '0' && p[0] <= '2')
        r.venta.pais = static_cast<uint8_t>(p[0] - '0');
    else
        fallar(Motivo::PAIS);

    if (!parsear_uuid(recortar(campos[1]), r.venta.cliente)) fallar(Motivo::UUID_CLIENTE);
    if (!r.boleta_valida) fallar(Motivo::UUID_BOLETA);

    switch (parsear_fecha_hora(recortar(campos[3]), r.venta.dia)) {
        case ResultadoFecha::FORMATO: fallar(Motivo::FECHA_FORMATO); break;
        case ResultadoFecha::CALENDARIO: fallar(Motivo::FECHA_CALENDARIO); break;
        case ResultadoFecha::OK:
            if (r.venta.dia < DIA_INICIO || r.venta.dia > DIA_FIN) fallar(Motivo::FECHA_RANGO);
            break;
    }

    switch (parsear_monto(recortar(campos[4]), r.venta.monto)) {
        case ResultadoMonto::NO_NUMERICO: fallar(Motivo::MONTO_NO_NUMERICO); break;
        case ResultadoMonto::NO_POSITIVO: fallar(Motivo::MONTO_NO_POSITIVO); break;
        case ResultadoMonto::OK: break;
    }
    return r;
}

}  // namespace ventas
