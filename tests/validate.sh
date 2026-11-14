#!/usr/bin/env bash
# Pruebas de validación SIN red ni credenciales (modo --validate-only).
# Uso: bash tests/validate.sh   (o make test)
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./programa
[[ -x "$BIN" ]] || make -s programa
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fallos=0
ok()   { echo "  ok   - $1"; }
mal()  { echo "  FALLA - $1"; fallos=$((fallos + 1)); }
espera() { if grep -q -- "$2" "$1"; then ok "$3"; else mal "$3"; fi; }

echo "Validación de data/ventas_ejemplo.csv"
env -u CPYD_EMAIL -u CPYD_APIKEY "$BIN" --input data/ventas_ejemplo.csv --output "$tmp/r.csv" --threads 2 --validate-only > "$tmp/log.txt"
E="$tmp/r.csv.errors.csv"
[[ "$(head -1 "$E")" == "linea,motivo,contenido" ]] && ok "encabezado del archivo de errores" || mal "encabezado del archivo de errores"
espera "$E" '^14,UUID de cliente inválido'                 "UUID de cliente inválido"
espera "$E" '^15,UUID de boleta inválido'                  "UUID de boleta inválido"
espera "$E" '^16,boleta duplicada: primera aparición en línea 3' "boleta duplicada (gana la primera aparición)"
espera "$E" '^32,boleta duplicada: primera aparición en línea 4' "boleta duplicada con otra fecha"
espera "$E" '^17,fecha fuera del rango'                    "fecha posterior al rango (2026-01-01)"
espera "$E" '^18,fecha fuera del rango'                    "fecha anterior al rango (2022-12-31)"
espera "$E" '^19,monto no positivo'                        "monto negativo"
espera "$E" '^20,monto no positivo'                        "monto cero"
espera "$E" '^21,monto no numérico'                        "monto no numérico (xyz)"
espera "$E" '^22,monto no numérico'                        "monto NaN"
espera "$E" '^24,"país inválido'                           "país 3"
espera "$E" '^25,"país inválido'                           "país -1"
espera "$E" '^26,fecha/hora inexistente'                   "29 de febrero en año no bisiesto"
espera "$E" '^27,fecha/hora inexistente'                   "mes 13"
espera "$E" '^28,fecha/hora con formato inválido'          "formato DD/MM/YYYY"
espera "$E" '^29,fecha/hora con formato inválido'          "fecha sin hora"
espera "$E" '^30,cantidad de campos'                       "faltan campos"
espera "$E" '^31,cantidad de campos'                       "sobran campos"
n=$(($(wc -l < "$E") - 1))
[[ "$n" -eq 19 ]] && ok "19 filas inválidas registradas" || mal "se esperaban 19 incidencias y hay $n"
espera "$tmp/log.txt" 'Ventas válidas: 9;'                  "9 ventas válidas"
espera "$tmp/log.txt" 'encabezado omitido: 1'              "encabezado detectado y omitido"
espera "$tmp/log.txt" 'únicos a consultar: 8'              "8 pares (país, fecha) únicos"
[[ ! -e "$tmp/r.csv" ]] && ok "--validate-only no genera resumen" || mal "--validate-only generó resumen"

echo "Determinismo con distinto número de hilos (archivo sintético)"
python3 -I scripts/generar_datos.py --filas 50000 --semilla 3 --salida "$tmp/g.csv" > /dev/null
for h in 1 3 8; do
  "$BIN" --input "$tmp/g.csv" --output "$tmp/g$h.csv" --threads "$h" --validate-only > "$tmp/g$h.log"
done
cmp -s "$tmp/g1.csv.errors.csv" "$tmp/g3.csv.errors.csv" && cmp -s "$tmp/g1.csv.errors.csv" "$tmp/g8.csv.errors.csv" \
  && ok "mismo archivo de errores con 1, 3 y 8 hilos" || mal "el archivo de errores depende del número de hilos"

echo "Delimitador ';' y CRLF"
sed 's/,/;/g; s/$/\r/' data/ventas_ejemplo.csv > "$tmp/pc.csv"
"$BIN" --input "$tmp/pc.csv" --output "$tmp/pc_out.csv" --validate-only > "$tmp/pc.log"
espera "$tmp/pc.log" 'Delimitador: ;' "detecta ';'"
espera "$tmp/pc.log" 'Ventas válidas: 9;' "mismas 9 ventas válidas"

echo "Errores globales"
if "$BIN" --input "$tmp/no_existe.csv" --validate-only 2>/dev/null; then mal "archivo inexistente debería fallar"; else ok "archivo inexistente → código ≠ 0"; fi
if "$BIN" --threads 0 --validate-only 2>/dev/null; then mal "--threads 0 debería fallar"; else ok "--threads 0 rechazado"; fi
if env -u CPYD_EMAIL -u CPYD_APIKEY "$BIN" --input data/ventas_ejemplo.csv --output "$tmp/z.csv" 2>"$tmp/z.err" >/dev/null; then
  mal "sin credenciales debería fallar"
else
  grep -q CPYD_EMAIL "$tmp/z.err" && ok "sin credenciales → mensaje claro y código ≠ 0" || mal "mensaje sin credenciales"
fi

if [[ $fallos -eq 0 ]]; then echo "Pruebas de validación: OK"; else echo "Pruebas de validación: $fallos FALLA(S)"; exit 1; fi
