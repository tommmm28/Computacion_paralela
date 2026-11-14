// api.hpp — Cliente de la API CPYD (autenticación JWT + tipo de cambio) con libcurl.
//
// Seguridad de hilos:
//   * ClienteHttp: un handle CURL por hilo (los handles de libcurl NO se
//     pueden compartir entre hilos). Reutilizar el handle mantiene viva la
//     conexión HTTPS (keep-alive) y evita repetir el handshake TLS.
//   * GestorToken: el JWT se comparte entre hilos protegido por std::mutex.
//     Un contador de "generación" evita que N hilos que reciben 401 al mismo
//     tiempo hagan N re-autenticaciones: solo el primero refresca, los demás
//     ven la generación nueva y reintentan con el token ya renovado.
//   * ApiCambio: sin estado mutable salvo contadores std::atomic.
//
// El token y las credenciales nunca se imprimen ni se escriben a disco.
#pragma once

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "cache_tasas.hpp"
#include "json_min.hpp"
#include "validacion.hpp"

namespace ventas {

constexpr const char* URL_API_POR_DEFECTO = "https://api.sebastian.cl/cpyd/v1";

struct ConfigApi {
    std::string base_url = URL_API_POR_DEFECTO;
    long timeout_s = 30;         // tiempo máximo total por petición
    long connect_timeout_s = 10; // tiempo máximo para conectar
    int reintentos = 3;          // reintentos ante errores transitorios (red, 429, 5xx)
    long backoff_ms = 250;       // espera base del backoff exponencial
};

struct RespuestaHttp {
    bool red_ok = false;     // false = timeout, DNS, conexión rechazada, TLS, etc.
    long status = 0;
    long retry_after_s = 0;  // cabecera Retry-After (si existe)
    std::string cuerpo;
    std::string error_red;
};

// Inicialización global de libcurl (una vez, antes de crear hilos).
class CurlGlobal {
public:
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("curl_global_init falló");
    }
    ~CurlGlobal() { curl_global_cleanup(); }
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
};

// Borrado "best effort" de secretos en memoria antes de liberar.
inline void borrar_secreto(std::string& s) {
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = '\0';
    s.clear();
}

class ClienteHttp {
public:
    explicit ClienteHttp(const ConfigApi& cfg) : cfg_(cfg), h_(curl_easy_init()) {}
    ~ClienteHttp() {
        if (h_) curl_easy_cleanup(h_);
    }
    ClienteHttp(const ClienteHttp&) = delete;
    ClienteHttp& operator=(const ClienteHttp&) = delete;

    RespuestaHttp get(const std::string& url, const std::string& token) { return ejecutar(url, nullptr, token); }
    RespuestaHttp post_json(const std::string& url, const std::string& cuerpo) { return ejecutar(url, &cuerpo, {}); }

private:
    static constexpr size_t MAX_CUERPO = 1 << 20;  // 1 MiB: protege contra respuestas anómalas

    static size_t escribir(char* p, size_t s, size_t n, void* u) {
        auto* b = static_cast<std::string*>(u);
        const size_t total = s * n;
        if (b->size() + total > MAX_CUERPO) return 0;  // aborta la transferencia
        b->append(p, total);
        return total;
    }

    RespuestaHttp ejecutar(const std::string& url, const std::string* cuerpo, const std::string& token) {
        RespuestaHttp r;
        if (!h_) {
            r.error_red = "curl_easy_init falló";
            return r;
        }
        // reset conserva las conexiones vivas y la caché de DNS/TLS del handle.
        curl_easy_reset(h_);
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_slist* cab = nullptr;
        cab = curl_slist_append(cab, "accept: application/json");
        std::string auth;
        if (!token.empty()) {
            auth = "Authorization: Bearer " + token;
            cab = curl_slist_append(cab, auth.c_str());
        }
        if (cuerpo) cab = curl_slist_append(cab, "Content-Type: application/json");

        curl_easy_setopt(h_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h_, CURLOPT_HTTPHEADER, cab);
        curl_easy_setopt(h_, CURLOPT_WRITEFUNCTION, &ClienteHttp::escribir);
        curl_easy_setopt(h_, CURLOPT_WRITEDATA, &r.cuerpo);
        curl_easy_setopt(h_, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(h_, CURLOPT_TIMEOUT, cfg_.timeout_s);
        curl_easy_setopt(h_, CURLOPT_CONNECTTIMEOUT, cfg_.connect_timeout_s);
        curl_easy_setopt(h_, CURLOPT_NOSIGNAL, 1L);  // obligatorio en programas multihilo
        curl_easy_setopt(h_, CURLOPT_USERAGENT, "cpyd-ventas-openmp/1.0");
        curl_easy_setopt(h_, CURLOPT_TCP_KEEPALIVE, 1L);
        if (cuerpo) {
            curl_easy_setopt(h_, CURLOPT_POST, 1L);
            curl_easy_setopt(h_, CURLOPT_POSTFIELDS, cuerpo->c_str());
            curl_easy_setopt(h_, CURLOPT_POSTFIELDSIZE, static_cast<long>(cuerpo->size()));
        }

        const CURLcode rc = curl_easy_perform(h_);
        if (rc == CURLE_OK) {
            r.red_ok = true;
            curl_easy_getinfo(h_, CURLINFO_RESPONSE_CODE, &r.status);
#if LIBCURL_VERSION_NUM >= 0x074200  // 7.66.0
            curl_off_t ra = 0;
            if (curl_easy_getinfo(h_, CURLINFO_RETRY_AFTER, &ra) == CURLE_OK && ra > 0)
                r.retry_after_s = static_cast<long>(ra);
#endif
        } else {
            r.error_red = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        }
        curl_easy_setopt(h_, CURLOPT_ERRORBUFFER, nullptr);  // errbuf deja de existir
        curl_easy_setopt(h_, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(cab);
        borrar_secreto(auth);
        return r;
    }

    const ConfigApi& cfg_;
    CURL* h_;
};

inline void esperar_backoff(const ConfigApi& cfg, int intento, long retry_after_s) {
    long ms = cfg.backoff_ms << std::min(intento, 5);
    ms = std::min(ms, 8000L);
    if (retry_after_s > 0) ms = std::max(ms, std::min(retry_after_s, 30L) * 1000L);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline bool error_transitorio(const RespuestaHttp& r) {
    return !r.red_ok || r.status == 429 || r.status == 408 || r.status >= 500;
}

inline std::string recortar_para_log(const std::string& s, size_t max = 160) {
    std::string o;
    for (char c : s.substr(0, max)) o += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    if (s.size() > max) o += "...";
    return o;
}

// ---------------------------------------------------------------------------
class GestorToken {
public:
    GestorToken(std::string email, std::string api_key, const ConfigApi& cfg)
        : email_(std::move(email)), api_key_(std::move(api_key)), cfg_(cfg) {}
    ~GestorToken() {
        borrar_secreto(token_);
        borrar_secreto(email_);
        borrar_secreto(api_key_);
    }
    GestorToken(const GestorToken&) = delete;
    GestorToken& operator=(const GestorToken&) = delete;

    // Se llama UNA vez al inicio, antes de la región paralela. Lanza si falla.
    void autenticar_inicial(ClienteHttp& http) {
        std::lock_guard<std::mutex> l(m_);
        token_ = solicitar_token(http);
        ++generacion_;
    }

    // Copia del token vigente y su generación.
    std::pair<std::string, uint64_t> actual() {
        std::lock_guard<std::mutex> l(m_);
        return {token_, generacion_};
    }

    // Refresca solo si nadie lo hizo desde que el hilo leyó `gen_vista`.
    // Se mantiene el mutex durante la petición a propósito: los demás hilos
    // esperan al token nuevo en lugar de lanzar autenticaciones duplicadas.
    void refrescar(uint64_t gen_vista, ClienteHttp& http) {
        std::lock_guard<std::mutex> l(m_);
        if (generacion_ != gen_vista) return;  // otro hilo ya lo renovó
        std::string nuevo = solicitar_token(http);
        borrar_secreto(token_);
        token_ = std::move(nuevo);
        ++generacion_;
        refrescos_.fetch_add(1, std::memory_order_relaxed);
    }

    long refrescos() const { return refrescos_.load(); }
    long autenticaciones() const { return autenticaciones_.load(); }

private:
    std::string solicitar_token(ClienteHttp& http) {
        const std::string url = cfg_.base_url + "/login/authenticate";
        std::string cuerpo = "{\"email\":\"" + json::escapar(email_) + "\",\"apiKey\":\"" + json::escapar(api_key_) + "\"}";
        RespuestaHttp r;
        for (int intento = 0;; ++intento) {
            r = http.post_json(url, cuerpo);
            autenticaciones_.fetch_add(1, std::memory_order_relaxed);
            if (error_transitorio(r) && intento < cfg_.reintentos) {
                esperar_backoff(cfg_, intento, r.retry_after_s);
                continue;
            }
            break;
        }
        borrar_secreto(cuerpo);
        if (!r.red_ok) throw std::runtime_error("autenticación: error de red: " + r.error_red);
        if (r.status == 401 || r.status == 403)
            throw std::runtime_error("autenticación rechazada (HTTP " + std::to_string(r.status) +
                                     "): revise CPYD_EMAIL y CPYD_APIKEY");
        if (r.status < 200 || r.status >= 300)
            throw std::runtime_error("autenticación: HTTP " + std::to_string(r.status));
        json::Valor v;
        std::string err;
        if (!json::parsear(r.cuerpo, v, err)) throw std::runtime_error("autenticación: respuesta JSON inválida: " + err);
        const json::Valor* jwt = v.buscar("jwt");
        if (!jwt || jwt->tipo != json::Tipo::CADENA || jwt->cadena.empty())
            throw std::runtime_error("autenticación: la respuesta no contiene el campo 'jwt'");
        std::string t = jwt->cadena;
        borrar_secreto(r.cuerpo);
        return t;
    }

    std::mutex m_;
    std::string email_, api_key_, token_;
    uint64_t generacion_ = 0;
    const ConfigApi& cfg_;
    std::atomic<long> refrescos_{0};
    std::atomic<long> autenticaciones_{0};
};

// ---------------------------------------------------------------------------
class ApiCambio {
public:
    ApiCambio(const ConfigApi& cfg, GestorToken& tok) : cfg_(cfg), tok_(tok) {}

    // Consulta la tasa de (país, día) y deja el resultado en `e`.
    // Nunca lanza excepciones (se ejecuta dentro de std::call_once / OpenMP).
    void consultar(int pais, int32_t dia, ClienteHttp& http, EntradaTasa& e) noexcept {
        try {
            consultar_impl(pais, dia, http, e);
        } catch (const std::exception& ex) {
            e.estado = EstadoTasa::ERROR;
            e.detalle = std::string("excepción: ") + ex.what();
        } catch (...) {
            e.estado = EstadoTasa::ERROR;
            e.detalle = "excepción desconocida";
        }
    }

    long llamadas() const { return llamadas_.load(); }
    long reintentos() const { return reintentos_.load(); }

private:
    void consultar_impl(int pais, int32_t dia, ClienteHttp& http, EntradaTasa& e) {
        const std::string url = cfg_.base_url + "/exchanges/" + pais_api(pais) + "/" + fecha_iso(dia);
        bool refrescado = false;
        int intento = 0;
        for (;;) {
            auto [token, gen] = tok_.actual();
            RespuestaHttp r = http.get(url, token);
            borrar_secreto(token);
            llamadas_.fetch_add(1, std::memory_order_relaxed);
            e.http = r.status;

            if (r.red_ok && r.status == 401) {  // token expirado/inválido: refrescar UNA vez
                if (!refrescado) {
                    refrescado = true;
                    try {
                        tok_.refrescar(gen, http);
                    } catch (const std::exception& ex) {
                        e.estado = EstadoTasa::ERROR;
                        e.detalle = std::string("HTTP 401 y no se pudo refrescar el token: ") + ex.what();
                        return;
                    }
                    continue;
                }
                e.estado = EstadoTasa::ERROR;
                e.detalle = "HTTP 401 persistente después de refrescar el token";
                return;
            }
            if (error_transitorio(r)) {
                if (intento < cfg_.reintentos) {
                    reintentos_.fetch_add(1, std::memory_order_relaxed);
                    esperar_backoff(cfg_, intento++, r.retry_after_s);
                    continue;
                }
                e.estado = EstadoTasa::ERROR;
                e.detalle = r.red_ok ? "HTTP " + std::to_string(r.status) + " tras " + std::to_string(intento + 1) + " intentos"
                                     : "error de red/timeout tras " + std::to_string(intento + 1) + " intentos: " + r.error_red;
                return;
            }
            if (r.status == 404 || r.status == 204) {
                e.estado = EstadoTasa::SIN_TASA;
                e.detalle = "la API no tiene tasa para esa fecha (HTTP " + std::to_string(r.status) + ")";
                return;
            }
            if (r.status < 200 || r.status >= 300) {
                e.estado = EstadoTasa::ERROR;
                e.detalle = "HTTP " + std::to_string(r.status) + ": " + recortar_para_log(r.cuerpo);
                return;
            }
            interpretar_cuerpo(r.cuerpo, e);
            return;
        }
    }

    static void interpretar_cuerpo(const std::string& cuerpo, EntradaTasa& e) {
        json::Valor v;
        std::string err;
        if (!json::parsear(cuerpo, v, err)) {
            e.estado = EstadoTasa::ERROR;
            e.detalle = "respuesta JSON inválida: " + err;
            return;
        }
        const json::Valor* rate = v.buscar("rate");
        if (!rate || rate->tipo == json::Tipo::NULO) {
            e.estado = EstadoTasa::SIN_TASA;
            e.detalle = "respuesta sin campo 'rate'";
            return;
        }
        double x = 0.0;
        if (rate->tipo == json::Tipo::NUMERO) {
            x = rate->numero;
        } else if (rate->tipo == json::Tipo::CADENA) {  // tolerancia: "940.58"
            const auto& s = rate->cadena;
            const auto res = std::from_chars(s.data(), s.data() + s.size(), x);
            if (res.ec != std::errc() || res.ptr != s.data() + s.size()) {
                e.estado = EstadoTasa::ERROR;
                e.detalle = "campo 'rate' no numérico";
                return;
            }
        } else {
            e.estado = EstadoTasa::ERROR;
            e.detalle = "campo 'rate' de tipo inesperado";
            return;
        }
        if (!std::isfinite(x) || x <= 0.0) {
            e.estado = EstadoTasa::ERROR;
            e.detalle = "tasa no positiva o no finita";
            return;
        }
        e.estado = EstadoTasa::OK;
        e.tasa = x;
    }

    const ConfigApi& cfg_;
    GestorToken& tok_;
    std::atomic<long> llamadas_{0};
    std::atomic<long> reintentos_{0};
};

}  // namespace ventas
