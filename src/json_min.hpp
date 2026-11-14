// json_min.hpp — Parser JSON mínimo (RFC 8259) sin dependencias externas.
//
// Decisión: no se usa nlohmann/json para no depender de una biblioteca que
// puede no estar instalada en la máquina de corrección. La API solo exige
// leer "jwt" y "rate", pero en vez de usar expresiones regulares se parsea
// el documento completo: así se detectan respuestas truncadas o inválidas y
// se manejan correctamente escapes, null, números con exponente, etc.
#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace json {

enum class Tipo { NULO, BOOLEANO, NUMERO, CADENA, OBJETO, ARREGLO };

struct Valor {
    Tipo tipo = Tipo::NULO;
    bool booleano = false;
    double numero = 0.0;
    std::string cadena;
    std::map<std::string, Valor> objeto;
    std::vector<Valor> arreglo;

    // Miembro directo de un objeto (nullptr si no existe o no es objeto).
    const Valor* miembro(const std::string& clave) const {
        if (tipo != Tipo::OBJETO) return nullptr;
        auto it = objeto.find(clave);
        return it == objeto.end() ? nullptr : &it->second;
    }
    // Búsqueda en profundidad (primero el nivel actual, luego anidados).
    const Valor* buscar(const std::string& clave, int profundidad = 4) const {
        if (const Valor* v = miembro(clave)) return v;
        if (profundidad == 0) return nullptr;
        if (tipo == Tipo::OBJETO) {
            for (const auto& kv : objeto)
                if (const Valor* v = kv.second.buscar(clave, profundidad - 1)) return v;
        } else if (tipo == Tipo::ARREGLO) {
            for (const auto& e : arreglo)
                if (const Valor* v = e.buscar(clave, profundidad - 1)) return v;
        }
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool parsear(Valor& out, std::string& error) {
        out = Valor{};
        espacios();
        if (!valor(out, 0)) {
            error = error_.empty() ? "JSON inválido" : error_;
            return false;
        }
        espacios();
        if (i_ != s_.size()) {
            error = "contenido extra después del JSON";
            return false;
        }
        return true;
    }

private:
    static constexpr int MAX_PROFUNDIDAD = 64;
    std::string_view s_;
    size_t i_ = 0;
    std::string error_;

    bool fallo(const char* m) {
        if (error_.empty()) error_ = std::string(m) + " (posición " + std::to_string(i_) + ")";
        return false;
    }
    void espacios() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool literal(const char* lit) {
        const std::string_view l(lit);
        if (s_.substr(i_, l.size()) != l) return fallo("literal inválido");
        i_ += l.size();
        return true;
    }

    bool valor(Valor& v, int prof) {
        if (prof > MAX_PROFUNDIDAD) return fallo("anidamiento excesivo");
        if (i_ >= s_.size()) return fallo("fin inesperado");
        const char c = s_[i_];
        if (c == '{') return objeto(v, prof);
        if (c == '[') return arreglo(v, prof);
        if (c == '"') {
            v.tipo = Tipo::CADENA;
            return cadena(v.cadena);
        }
        if (c == 't') { v.tipo = Tipo::BOOLEANO; v.booleano = true; return literal("true"); }
        if (c == 'f') { v.tipo = Tipo::BOOLEANO; v.booleano = false; return literal("false"); }
        if (c == 'n') { v.tipo = Tipo::NULO; return literal("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) return numero(v);
        return fallo("carácter inesperado");
    }

    bool objeto(Valor& v, int prof) {
        v.tipo = Tipo::OBJETO;
        v.objeto.clear();
        ++i_;  // '{'
        espacios();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            espacios();
            if (i_ >= s_.size() || s_[i_] != '"') return fallo("se esperaba clave");
            std::string clave;
            if (!cadena(clave)) return false;
            espacios();
            if (i_ >= s_.size() || s_[i_] != ':') return fallo("se esperaba ':'");
            ++i_;
            espacios();
            Valor hijo;
            if (!valor(hijo, prof + 1)) return false;
            v.objeto[std::move(clave)] = std::move(hijo);
            espacios();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            return fallo("se esperaba ',' o '}'");
        }
    }

    bool arreglo(Valor& v, int prof) {
        v.tipo = Tipo::ARREGLO;
        v.arreglo.clear();
        ++i_;  // '['
        espacios();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            espacios();
            Valor hijo;
            if (!valor(hijo, prof + 1)) return false;
            v.arreglo.push_back(std::move(hijo));
            espacios();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            return fallo("se esperaba ',' o ']'");
        }
    }

    static void utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) {
            o += static_cast<char>(cp);
        } else if (cp < 0x800) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& cp) {
        if (i_ + 4 > s_.size()) return fallo("escape \\u incompleto");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) return fallo("escape \\u inválido");
            cp = (cp << 4) | static_cast<uint32_t>(d);
        }
        return true;
    }

    bool cadena(std::string& out) {
        ++i_;  // '"'
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fallo("carácter de control en cadena");
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) break;
            const char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // par sustituto
                        uint32_t bajo;
                        if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return fallo("sustituto sin pareja");
                        i_ += 2;
                        if (!hex4(bajo) || bajo < 0xDC00 || bajo > 0xDFFF) return fallo("sustituto inválido");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (bajo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: return fallo("escape inválido");
            }
        }
        return fallo("cadena sin cerrar");
    }

    bool numero(Valor& v) {
        const size_t ini = i_;
        if (s_[i_] == '-') ++i_;
        if (i_ >= s_.size()) return fallo("número incompleto");
        if (s_[i_] == '0') {
            ++i_;
        } else if (s_[i_] >= '1' && s_[i_] <= '9') {
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        } else {
            return fallo("número inválido");
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            const size_t d = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            if (i_ == d) return fallo("fracción vacía");
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            const size_t d = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            if (i_ == d) return fallo("exponente vacío");
        }
        v.tipo = Tipo::NUMERO;
        const auto r = std::from_chars(s_.data() + ini, s_.data() + i_, v.numero);
        if (r.ec != std::errc() || r.ptr != s_.data() + i_) return fallo("número fuera de rango");
        return true;
    }
};

inline bool parsear(std::string_view texto, Valor& out, std::string& error) {
    return Parser(texto).parsear(out, error);
}

// Escapa una cadena para incluirla dentro de un literal JSON.
inline std::string escapar(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (const char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

}  // namespace json
