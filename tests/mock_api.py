#!/usr/bin/env python3
"""API CPYD SIMULADA, solo para pruebas locales sin credenciales reales.

Imita los dos endpoints usados por el programa:
  POST /cpyd/v1/login/authenticate          -> {"jwt": "..."}
  GET  /cpyd/v1/exchanges/{country}/{date}  -> {"country":..., "date":..., "rate":...}

Las tasas que entrega son VALORES SINTÉTICOS DE PRUEBA (fórmula determinista
abajo), sin relación con tasas reales. El programa nunca las usa fuera de
estas pruebas: en uso normal consulta https://api.sebastian.cl.

Permite simular: token expirado (401), fechas sin tasa (404), errores 5xx
transitorios, respuestas lentas (timeout), JSON inválido y tasas no positivas.
"""
import datetime as dt
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PREFIJO = "/cpyd/v1"
BASE_PRUEBA = {"chile": 900.0, "colombia": 4000.0, "ecuador": 1.0}


def tasa_sintetica(pais: str, fecha: str) -> float:
    """Tasa de PRUEBA (no real): base * (1 + (ordinal % 17) / 100)."""
    o = dt.date.fromisoformat(fecha).toordinal()
    return round(BASE_PRUEBA[pais] * (1 + (o % 17) / 100), 4)


class Estado:
    def __init__(self, email, api_key, expirar_tras=0, faltantes=(), inestables=None,
                 lentas=(), json_invalido=(), tasa_cero=(), demora_s=3.0, latencia_ms=0):
        self.email, self.api_key = email, api_key
        self.expirar_tras = expirar_tras          # nº de respuestas 200 antes de invalidar el token
        self.faltantes = set(faltantes)           # {(pais, fecha)} -> 404
        self.inestables = dict(inestables or {})  # {(pais, fecha): n} -> n respuestas 503 antes de 200
        self.lentas = set(lentas)                 # {(pais, fecha)} -> demora > timeout
        self.json_invalido = set(json_invalido)
        self.tasa_cero = set(tasa_cero)
        self.demora_s = demora_s
        self.latencia_s = latencia_ms / 1000.0    # latencia artificial por petición (simula red)
        self.lock = threading.Lock()
        self.tokens_validos = set()
        self.generacion = 0
        self.expirado = False
        self.autenticaciones = 0
        self.ok_por_clave = {}
        self.peticiones_por_clave = {}
        self.respuestas_401 = 0
        self.ok_total = 0


def crear_servidor(estado: Estado, puerto: int = 0) -> ThreadingHTTPServer:
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"  # keep-alive
        disable_nagle_algorithm = True  # evita esperas de 40 ms (Nagle + ACK retardado)

        def log_message(self, *args):  # silencioso
            pass

        def responder(self, codigo, cuerpo, tipo="application/json"):
            datos = cuerpo.encode() if isinstance(cuerpo, str) else json.dumps(cuerpo).encode()
            self.send_response(codigo)
            self.send_header("Content-Type", tipo)
            self.send_header("Content-Length", str(len(datos)))
            self.end_headers()
            self.wfile.write(datos)

        def do_POST(self):
            largo = int(self.headers.get("Content-Length", "0"))
            cuerpo = self.rfile.read(largo)
            if self.path != PREFIJO + "/login/authenticate":
                return self.responder(404, {"error": "not found"})
            if self.headers.get("Content-Type", "").split(";")[0] != "application/json":
                return self.responder(415, {"error": "content-type"})
            try:
                datos = json.loads(cuerpo)
            except ValueError:
                return self.responder(400, {"error": "json"})
            with estado.lock:
                estado.autenticaciones += 1
                if datos.get("email") != estado.email or datos.get("apiKey") != estado.api_key:
                    return self.responder(401, {"error": "credenciales inválidas"})
                estado.generacion += 1
                token = f"tok-prueba-{estado.generacion}"
                estado.tokens_validos = {token}
            self.responder(200, {"jwt": token})

        def do_GET(self):
            partes = self.path.split("/")
            # ['', 'cpyd', 'v1', 'exchanges', pais, fecha]
            if len(partes) != 6 or "/".join(partes[:4]) != PREFIJO + "/exchanges":
                return self.responder(404, {"error": "not found"})
            pais, fecha = partes[4], partes[5]
            auth = self.headers.get("Authorization", "")
            clave = (pais, fecha)
            with estado.lock:
                estado.peticiones_por_clave[clave] = estado.peticiones_por_clave.get(clave, 0) + 1
                if not auth.startswith("Bearer ") or auth[7:] not in estado.tokens_validos:
                    estado.respuestas_401 += 1
                    return self.responder(401, {"error": "token inválido o expirado"})
                if pais not in BASE_PRUEBA:
                    return self.responder(400, {"error": "país"})
                try:
                    dt.date.fromisoformat(fecha)
                except ValueError:
                    return self.responder(400, {"error": "fecha"})
                if estado.inestables.get(clave, 0) > 0:
                    estado.inestables[clave] -= 1
                    return self.responder(503, {"error": "servicio no disponible"})
                lenta = clave in estado.lentas
            if estado.latencia_s:
                time.sleep(estado.latencia_s)
            if lenta:
                time.sleep(estado.demora_s)
            if clave in estado.faltantes:
                return self.responder(404, {"error": "sin tasa para la fecha"})
            if clave in estado.json_invalido:
                return self.responder(200, "<html>proxy error</html>", "text/html")
            tasa = 0 if clave in estado.tasa_cero else tasa_sintetica(pais, fecha)
            with estado.lock:
                estado.ok_total += 1
                estado.ok_por_clave[clave] = estado.ok_por_clave.get(clave, 0) + 1
                if estado.expirar_tras and not estado.expirado and estado.ok_total >= estado.expirar_tras:
                    estado.expirado = True
                    estado.tokens_validos = set()  # el token vigente "expira"
            self.responder(200, {"country": pais.capitalize(), "date": fecha, "rate": tasa})

    class Servidor(ThreadingHTTPServer):
        daemon_threads = True

        def handle_error(self, request, client_address):
            pass  # p. ej. el cliente cerró la conexión por timeout

    return Servidor(("127.0.0.1", puerto), Handler)


if __name__ == "__main__":
    # Uso manual (sin credenciales reales):
    #   python3 tests/mock_api.py --puerto 8099 --latencia-ms 30
    #   export CPYD_API_BASE=http://127.0.0.1:8099/cpyd/v1
    #   export CPYD_EMAIL=prueba@utem.cl CPYD_APIKEY=11.111.111-1
    #   ./programa --input data/ventas_ejemplo.csv --output /tmp/r.csv --threads 4
    import argparse

    ap = argparse.ArgumentParser(description="API CPYD simulada (solo pruebas)")
    ap.add_argument("--puerto", type=int, default=8099)
    ap.add_argument("--latencia-ms", type=float, default=0.0)
    ap.add_argument("--email", default="prueba@utem.cl")
    ap.add_argument("--apikey", default="11.111.111-1")
    ap.add_argument("--expirar-tras", type=int, default=0, help="invalida el token tras N respuestas 200")
    a = ap.parse_args()
    est = Estado(a.email, a.apikey, expirar_tras=a.expirar_tras, latencia_ms=a.latencia_ms)
    srv = crear_servidor(est, a.puerto)
    print(f"API simulada en http://127.0.0.1:{srv.server_address[1]}{PREFIJO} (Ctrl+C para terminar)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    print(f"autenticaciones={est.autenticaciones} respuestas_200={est.ok_total} respuestas_401={est.respuestas_401}")
