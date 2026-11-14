#!/usr/bin/env python3
"""Genera un CSV SINTÉTICO de ventas para pruebas y benchmark.

Solo genera transacciones (UUID, fechas y montos aleatorios). No genera ni
usa tasas de cambio: esas siempre provienen de la API.

Ejemplos:
  python3 scripts/generar_datos.py --filas 1000000 --salida data/ventas_grande.csv
  python3 scripts/generar_datos.py --filas 5000 --salida /tmp/v.csv --verdad /tmp/validas.csv

--verdad escribe las filas que DEBEN pasar la validación (pais,id_cliente,fecha,monto),
útil para verificar resultados en pruebas automáticas.
"""
import argparse
import datetime as dt
import random
import uuid

INICIO = dt.date(2023, 1, 1)
FIN = dt.date(2025, 12, 31)


def uuid4(rng: random.Random) -> str:
    return str(uuid.UUID(int=rng.getrandbits(128), version=4))


def fecha_hora(rng: random.Random, fecha: dt.date) -> str:
    sep = rng.choice((" ", "T"))
    return f"{fecha.isoformat()}{sep}{rng.randrange(24):02d}:{rng.randrange(60):02d}:{rng.randrange(60):02d}"


def monto(rng: random.Random, pais: int) -> str:
    if pais == 0:   # CLP: enteros
        return str(rng.randint(1_000, 2_000_000))
    if pais == 1:   # COP
        return f"{rng.randint(5_000, 10_000_000)}.{rng.randrange(100):02d}"
    return f"{rng.uniform(1, 5_000):.2f}"  # USD (Ecuador)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--filas", type=int, default=100_000)
    ap.add_argument("--semilla", type=int, default=42)
    ap.add_argument("--clientes", type=int, default=0, help="clientes distintos (por defecto filas/20)")
    ap.add_argument("--invalidas", type=float, default=0.02, help="proporción de filas inválidas")
    ap.add_argument("--fuera-rango", type=float, default=0.03, help="proporción con fecha fuera de rango")
    ap.add_argument("--duplicadas", type=float, default=0.01, help="proporción de boletas repetidas")
    ap.add_argument("--salida", required=True)
    ap.add_argument("--verdad")
    a = ap.parse_args()

    rng = random.Random(a.semilla)
    nclientes = a.clientes or max(1, a.filas // 20)
    clientes = [uuid4(rng) for _ in range(nclientes)]
    ndias = (FIN - INICIO).days + 1
    boletas_validas = []
    verdad = []

    with open(a.salida, "w", newline="") as out:
        for _ in range(a.filas):
            pais = rng.randrange(3)
            cliente = rng.choice(clientes)
            boleta = uuid4(rng)
            fecha = INICIO + dt.timedelta(days=rng.randrange(ndias))
            m = monto(rng, pais)
            x = rng.random()
            if x < a.invalidas:
                tipo = rng.randrange(9)
                campos = [str(pais), cliente, boleta, fecha_hora(rng, fecha), m]
                if tipo == 0: campos[0] = rng.choice(("3", "-1", "x", ""))
                elif tipo == 1: campos[1] = cliente[:-1]
                elif tipo == 2: campos[2] = "boleta-" + str(rng.randrange(10**6))
                elif tipo == 3: campos[3] = fecha.strftime("%d/%m/%Y 10:00:00")
                elif tipo == 4: campos[3] = f"{fecha.year}-02-30 10:00:00"
                elif tipo == 5: campos[4] = "-" + m
                elif tipo == 6: campos[4] = "0"
                elif tipo == 7: campos[4] = rng.choice(("abc", "NaN", "1,5", ""))
                else: campos = campos[:4]
                out.write(",".join(campos) + "\n")
            elif x < a.invalidas + a.fuera_rango:
                fuera = rng.choice((INICIO - dt.timedelta(days=rng.randint(1, 400)),
                                    FIN + dt.timedelta(days=rng.randint(1, 400))))
                out.write(f"{pais},{cliente},{boleta},{fecha_hora(rng, fuera)},{m}\n")
            elif x < a.invalidas + a.fuera_rango + a.duplicadas and boletas_validas:
                out.write(f"{pais},{cliente},{rng.choice(boletas_validas)},{fecha_hora(rng, fecha)},{m}\n")
            else:
                out.write(f"{pais},{cliente},{boleta},{fecha_hora(rng, fecha)},{m}\n")
                boletas_validas.append(boleta)
                verdad.append((pais, cliente, fecha.isoformat(), m))

    if a.verdad:
        with open(a.verdad, "w", newline="") as v:
            v.write("pais,id_cliente,fecha,monto\n")
            for fila in verdad:
                v.write(",".join(map(str, fila)) + "\n")
    print(f"{a.filas} filas escritas en {a.salida} ({len(verdad)} válidas esperadas)")


if __name__ == "__main__":
    main()
