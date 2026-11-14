// cache_tasas.hpp — Caché sincronizada de tasas por (país, fecha).
//
// Diseño: un arreglo de tamaño fijo (3 países × N días) indexado por
// (país, día). Cada entrada tiene un std::once_flag: la primera vez que un
// hilo pide una clave, std::call_once ejecuta la carga (consulta HTTP) y
// cualquier otro hilo que pida la misma clave al mismo tiempo ESPERA a que
// termine en vez de repetir la consulta. Después de la carga, las lecturas
// quedan sincronizadas por call_once (relación happens-before), sin mutex
// global ni contención entre claves distintas.
//
// El módulo no depende de libcurl: el "cargador" se inyecta como función, lo
// que permite probar la caché sin red (tests/unit_tests.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "validacion.hpp"

namespace ventas {

enum class EstadoTasa : uint8_t { PENDIENTE, OK, SIN_TASA, ERROR };

inline const char* texto_estado(EstadoTasa e) {
    switch (e) {
        case EstadoTasa::PENDIENTE: return "pendiente";
        case EstadoTasa::OK: return "ok";
        case EstadoTasa::SIN_TASA: return "sin_tasa";
        case EstadoTasa::ERROR: return "error";
    }
    return "?";
}

struct EntradaTasa {
    std::once_flag cargada;
    EstadoTasa estado = EstadoTasa::PENDIENTE;
    double tasa = 0.0;     // moneda local por 1 USD (solo si estado == OK)
    long http = 0;         // último código HTTP observado
    std::string detalle;   // descripción del problema (si lo hubo)
};

class CacheTasas {
public:
    CacheTasas(int32_t dia_min, int32_t dia_max)
        : dia_min_(dia_min),
          ndias_(static_cast<size_t>(dia_max - dia_min + 1)),
          entradas_(new EntradaTasa[static_cast<size_t>(NUM_PAISES) * ndias_]) {}

    size_t tamano() const { return static_cast<size_t>(NUM_PAISES) * ndias_; }
    int32_t dia_min() const { return dia_min_; }
    int32_t dia_max() const { return dia_min_ + static_cast<int32_t>(ndias_) - 1; }

    bool contiene(int pais, int32_t dia) const {
        return pais >= 0 && pais < NUM_PAISES && dia >= dia_min_ && dia <= dia_max();
    }
    size_t indice(int pais, int32_t dia) const {
        return static_cast<size_t>(pais) * ndias_ + static_cast<size_t>(dia - dia_min_);
    }
    int pais_de(size_t idx) const { return static_cast<int>(idx / ndias_); }
    int32_t dia_de(size_t idx) const { return dia_min_ + static_cast<int32_t>(idx % ndias_); }

    // Thread-safe. `cargar(EntradaTasa&)` se ejecuta como máximo una vez por clave
    // y NO debe lanzar excepciones.
    template <class Cargador>
    const EntradaTasa& obtener(size_t idx, Cargador&& cargar) {
        EntradaTasa& e = entradas_[idx];
        solicitudes_.fetch_add(1, std::memory_order_relaxed);
        std::call_once(e.cargada, [&] {
            cargas_.fetch_add(1, std::memory_order_relaxed);
            cargar(e);
            if (e.estado == EstadoTasa::PENDIENTE) {
                e.estado = EstadoTasa::ERROR;
                if (e.detalle.empty()) e.detalle = "cargador no asignó estado";
            }
        });
        return e;
    }

    // Lectura sin sincronización: usar SOLO fuera de regiones paralelas
    // (después de la barrera implícita que cierra la región).
    const EntradaTasa& entrada(size_t idx) const { return entradas_[idx]; }

    long solicitudes() const { return solicitudes_.load(); }
    long cargas() const { return cargas_.load(); }

private:
    int32_t dia_min_;
    size_t ndias_;
    std::unique_ptr<EntradaTasa[]> entradas_;
    std::atomic<long> solicitudes_{0};
    std::atomic<long> cargas_{0};
};

}  // namespace ventas
