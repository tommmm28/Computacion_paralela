#!/usr/bin/env python3
"""Prueba de extremo a extremo contra la API SIMULADA (tests/mock_api.py).

No usa red externa ni credenciales reales: levanta un servidor HTTP local en
127.0.0.1 y ejecuta ./programa con CPYD_API_BASE apuntando a él.

Verifica:
  * totales por (país, cliente) contra un cálculo independiente en Python;
  * resultados idénticos con 1, 2, 4 y 8 hilos;
  * una sola consulta exitosa por (país, fecha) (caché/deduplicación);
  * token expirado → un único refresco (2 autenticaciones en total) y reintento;
  * errores 503 transitorios → reintento exitoso;
  * fechas sin tasa (404) → registradas y omitidas; con --fallback-days, tasa anterior;
  * timeout, JSON inválido y tasa 0 → registrados como error de API;
  * credenciales incorrectas o ausentes → código de salida 1.

Uso: python3 tests/e2e_mock.py [--bin ./programa]
"""
import argparse
import csv
import datetime as dt
import importlib.util
import os
import random
import subprocess
import sys
import tempfile
import threading
import uuid
from collections import defaultdict
from decimal import Decimal, getcontext

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(AQUI)
spec = importlib.util.spec_from_file_location("mock_api", os.path.join(AQUI, "mock_api.py"))
mock_api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mock_api)

PAISES = ["chile", "colombia", "ecuador"]
EMAIL, CLAVE = "prueba@utem.cl", "11.111.111-1"   # credenciales FICTICIAS del servidor simulado
getcontext().prec = 40

fallos = 0


def check(cond, msg):
    global fallos
    if not cond:
        fallos += 1
        print(f"  FALLA: {msg}")


def dias(fecha, n):
    return (dt.date.fromisoformat(fecha) - dt.timedelta(days=n)).isoformat()


def esperado(verdad, faltantes, con_error, fallback):
    tot = defaultdict(Decimal)
    omitidas = 0
    for pais, cliente, fecha, monto in verdad:
        p = PAISES[pais]
        rate = None
        if (p, fecha) in con_error:
            pass
        elif (p, fecha) not in faltantes:
            rate = mock_api.tasa_sintetica(p, fecha)
        else:
            for r in range(1, fallback + 1):
                f2 = dias(fecha, r)
                if (p, f2) in con_error:
                    break
                if (p, f2) not in faltantes:
                    rate = mock_api.tasa_sintetica(p, f2)
                    break
        if rate is None:
            omitidas += 1
            continue
        tot[(pais, cliente.lower())] += Decimal(monto) / Decimal(repr(rate))
    return tot, omitidas


def leer_salida(ruta):
    with open(ruta, newline="") as f:
        filas = list(csv.reader(f))
    assert filas[0] == ["pais", "id_cliente", "total_usd"], filas[0]
    return filas[1:]


def comparar(salida, tot, etiqueta):
    check(len(salida) == len(tot), f"{etiqueta}: {len(salida)} filas vs {len(tot)} esperadas")
    claves = [(int(p), c) for p, c, _ in salida]
    check(claves == sorted(claves), f"{etiqueta}: salida no ordenada por país y cliente")
    peor = Decimal(0)
    for p, c, v in salida:
        e = tot.get((int(p), c))
        if e is None:
            check(False, f"{etiqueta}: clave inesperada {p},{c}")
            continue
        check(len(v.split(".")[1]) == 2, f"{etiqueta}: formato decimal {v}")
        peor = max(peor, abs(Decimal(v) - e))
    check(peor <= Decimal("0.0051"), f"{etiqueta}: diferencia máxima {peor}")
    return peor


def ejecutar(bin_, args, base, env_extra=None, email=EMAIL, clave=CLAVE):
    env = dict(os.environ)
    env.pop("CPYD_EMAIL", None)
    env.pop("CPYD_APIKEY", None)
    if email is not None:
        env["CPYD_EMAIL"] = email
    if clave is not None:
        env["CPYD_APIKEY"] = clave
    env["CPYD_API_BASE"] = base
    env["NO_PROXY"] = env["no_proxy"] = "127.0.0.1,localhost"
    env.update(env_extra or {})
    return subprocess.run([bin_] + args, env=env, capture_output=True, text=True, timeout=600)


def con_servidor(estado):
    srv = mock_api.crear_servidor(estado)
    hilo = threading.Thread(target=srv.serve_forever, daemon=True)
    hilo.start()
    return srv, f"http://127.0.0.1:{srv.server_address[1]}/cpyd/v1"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(RAIZ, "programa"))
    ap.add_argument("--filas", type=int, default=4000)
    a = ap.parse_args()
    bin_ = os.path.abspath(a.bin)
    tmp = tempfile.mkdtemp(prefix="cpyd_e2e_")
    ventas, verdad_csv = os.path.join(tmp, "ventas.csv"), os.path.join(tmp, "validas.csv")
    subprocess.run([sys.executable, "-I", os.path.join(RAIZ, "scripts", "generar_datos.py"), "--filas", str(a.filas),
                    "--semilla", "7", "--salida", ventas, "--verdad", verdad_csv], check=True, capture_output=True)
    with open(verdad_csv, newline="") as f:
        verdad = [(int(r[0]), r[1], r[2], r[3]) for r in list(csv.reader(f))[1:]]

    # Fechas especiales (se agregan ventas válidas en ellas para asegurar cobertura)
    faltantes = {("chile", "2024-03-15")} | {("ecuador", f"2025-02-{d:02d}") for d in range(10, 15)}
    inestables = {("colombia", "2024-07-01"): 2}
    lentas, json_malo, cero = {("chile", "2023-05-05")}, {("colombia", "2023-08-08")}, {("ecuador", "2023-09-09")}
    rng = random.Random(99)
    extra = []
    for p, f in sorted(faltantes) + list(inestables) + sorted(lentas | json_malo | cero) + [("chile", "2024-03-14")]:
        for _ in range(3):
            pais = PAISES.index(p)
            cli = str(uuid.UUID(int=rng.getrandbits(128), version=4))
            m = f"{rng.randint(100, 99999)}.{rng.randrange(100):02d}"
            extra.append((pais, cli, f, m))
    # Un mismo cliente en dos países (no deben mezclarse)
    compartido = str(uuid.UUID(int=rng.getrandbits(128), version=4))
    extra += [(0, compartido, "2024-01-10", "1000"), (2, compartido, "2024-01-10", "50.00")]
    with open(ventas, "a") as f:
        for pais, cli, fecha, m in extra:
            f.write(f"{pais},{cli},{uuid.UUID(int=rng.getrandbits(128), version=4)},{fecha}T12:00:00,{m}\n")
    verdad += extra
    print(f"Datos de prueba: {a.filas + len(extra)} filas, {len(verdad)} válidas esperadas ({tmp})")

    # ---- Escenario A: expiración de token, 503 transitorios, 404; 1/2/4/8 hilos
    print("Escenario A: 401→refresco, 503→reintento, 404→omitida; 1, 2, 4 y 8 hilos")
    tot, omit = esperado(verdad, faltantes, set(), 0)
    salidas = {}
    for h in (1, 2, 4, 8):
        estado = mock_api.Estado(EMAIL, CLAVE, expirar_tras=40, faltantes=faltantes, inestables=inestables)
        srv, base = con_servidor(estado)
        out = os.path.join(tmp, f"resumen_{h}.csv")
        r = ejecutar(bin_, ["--input", ventas, "--output", out, "--threads", str(h)], base)
        srv.shutdown()
        check(r.returncode == 0, f"A/{h}: código {r.returncode}: {r.stderr.strip()}")
        if r.returncode != 0:
            continue
        salidas[h] = open(out).read()
        peor = comparar(leer_salida(out), tot, f"A/{h}")
        check(estado.autenticaciones == 2, f"A/{h}: autenticaciones={estado.autenticaciones} (esperado 2)")
        check(max(estado.ok_por_clave.values()) == 1, f"A/{h}: alguna (país,fecha) se consultó con éxito más de una vez")
        with open(out + ".errors.csv", newline="") as f:
            errores = list(csv.reader(f))[1:]
        sin_tasa = sum(1 for e in errores if e[1].startswith("sin tasa disponible"))
        check(sin_tasa == omit, f"A/{h}: {sin_tasa} filas 'sin tasa' vs {omit} esperadas")
        check("tok-prueba" not in r.stdout + r.stderr, f"A/{h}: ¡el token aparece en la salida!")
        print(f"  {h} hilo(s): OK  (dif. máx {peor}, 401 recibidos {estado.respuestas_401}, "
              f"peticiones {sum(estado.peticiones_por_clave.values())})")
    check(len(set(salidas.values())) == 1, "A: los resúmenes difieren según el número de hilos")

    # ---- Escenario B: fallback a la última tasa anterior
    print("Escenario B: --fallback-days 3")
    tot_b, omit_b = esperado(verdad, faltantes, set(), 3)
    estado = mock_api.Estado(EMAIL, CLAVE, faltantes=faltantes)
    srv, base = con_servidor(estado)
    out = os.path.join(tmp, "resumen_fallback.csv")
    r = ejecutar(bin_, ["--input", ventas, "--output", out, "--threads", "4", "--fallback-days", "3"], base)
    srv.shutdown()
    check(r.returncode == 0, f"B: código {r.returncode}: {r.stderr.strip()}")
    if r.returncode == 0:
        comparar(leer_salida(out), tot_b, "B")
        with open(out + ".errors.csv", newline="") as f:
            sin_tasa = sum(1 for e in list(csv.reader(f))[1:] if e[1].startswith("sin tasa disponible"))
        check(sin_tasa == omit_b, f"B: {sin_tasa} filas 'sin tasa' vs {omit_b} esperadas")
        with open(out + ".tasas.csv") as f:
            check("fallback: se usó la tasa de 2024-03-14" in f.read(), "B: bitácora de tasas sin fallback")
        print(f"  OK (omitidas sin tasa: {omit_b} de {omit} sin fallback)")

    # ---- Escenario C: timeout, JSON inválido y tasa 0 → error de API
    print("Escenario C: timeout / JSON inválido / tasa 0 (--timeout 1 --retries 1)")
    estado = mock_api.Estado(EMAIL, CLAVE, lentas=lentas, json_invalido=json_malo, tasa_cero=cero, demora_s=2.5)
    srv, base = con_servidor(estado)
    out = os.path.join(tmp, "resumen_errores.csv")
    r = ejecutar(bin_, ["--input", ventas, "--output", out, "--threads", "4", "--timeout", "1", "--retries", "1"], base)
    srv.shutdown()
    check(r.returncode == 0, f"C: código {r.returncode}: {r.stderr.strip()}")
    if r.returncode == 0:
        tot_c, _ = esperado(verdad, set(), lentas | json_malo | cero, 0)
        comparar(leer_salida(out), tot_c, "C")
        with open(out + ".errors.csv", newline="") as f:
            errores = [e[1] for e in list(csv.reader(f))[1:] if e[1].startswith("error de API")]
        check(any("timeout" in e.lower() or "timed out" in e.lower() for e in errores), "C: no se registró el timeout")
        check(any("JSON inválida" in e for e in errores), "C: no se registró el JSON inválido")
        check(any("no positiva" in e for e in errores), "C: no se registró la tasa 0")
        print(f"  OK ({len(errores)} ventas omitidas por error de API)")

    # ---- Escenario D: credenciales
    print("Escenario D: credenciales incorrectas / ausentes")
    estado = mock_api.Estado(EMAIL, CLAVE)
    srv, base = con_servidor(estado)
    r = ejecutar(bin_, ["--input", ventas, "--output", os.path.join(tmp, "x.csv")], base, clave="00.000.000-0")
    check(r.returncode == 1 and "rechazada" in r.stderr, f"D: credenciales malas → {r.returncode} {r.stderr.strip()}")
    check("00.000.000-0" not in r.stdout + r.stderr, "D: la clave aparece en la salida")
    r = ejecutar(bin_, ["--input", ventas, "--output", os.path.join(tmp, "x.csv")], base, email=None)
    check(r.returncode == 1 and "CPYD_EMAIL" in r.stderr, f"D: sin variables → {r.returncode}")
    srv.shutdown()
    print("  OK" if fallos == 0 else "  (ver fallas)")

    print("Prueba e2e con API simulada: " + ("OK" if fallos == 0 else f"{fallos} FALLA(S)"))
    sys.exit(1 if fallos else 0)


if __name__ == "__main__":
    main()
