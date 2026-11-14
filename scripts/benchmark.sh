#!/usr/bin/env bash
# Benchmark: ejecuta ./programa con 1, 2, 4 y 8 hilos y calcula
#   speedup S(p) = T1 / Tp      eficiencia E(p) = S(p) / p
#
# Uso:
#   scripts/benchmark.sh [ENTRADA] [DIR_RESULTADOS]
#
# Variables opcionales:
#   MODO=completo      (por defecto) pipeline completo con la API real; requiere
#                      CPYD_EMAIL y CPYD_APIKEY en el entorno.
#   MODO=validacion    solo lectura + validación (--validate-only), sin red.
#   HILOS="1 2 4 8"    lista de hilos a medir.
#   REPETICIONES=N     repeticiones por cantidad de hilos (se usa la mediana).
#                      Por defecto 1 en modo completo (cada corrida hace ~1 petición
#                      HTTP por (país, fecha); no sobrecargar la API) y 3 en validación.
#   PAUSA=S            segundos de espera entre corridas (por defecto 0).
#   EXTRA="..."        argumentos adicionales para ./programa (p. ej. "--fallback-days 3").
#
# Resultados en DIR_RESULTADOS (por defecto benchmark-results/<modo>-<fecha>):
#   corridas.csv  tiempos de cada corrida (pared y por fase)
#   resumen.csv   mediana, speedup y eficiencia por cantidad de hilos
#   resumen.md    la misma tabla en Markdown, lista para pegar en el README
#   entorno.txt   CPU, núcleos, compilador (para documentar el experimento)
set -euo pipefail
cd "$(dirname "$0")/.."

MODO="${MODO:-completo}"
ENTRADA="${1:-ventas.csv}"
SALIDA="${2:-benchmark-results/${MODO}-$(date +%Y%m%d-%H%M%S)}"
HILOS="${HILOS:-1 2 4 8}"
PAUSA="${PAUSA:-0}"
EXTRA="${EXTRA:-}"
if [[ "$MODO" == "completo" ]]; then REPETICIONES="${REPETICIONES:-1}"; else REPETICIONES="${REPETICIONES:-3}"; fi

[[ -x ./programa ]] || { echo "Falta ./programa: ejecute 'make' primero." >&2; exit 1; }
[[ -f "$ENTRADA" ]] || { echo "No existe el archivo de entrada: $ENTRADA" >&2; exit 1; }
case "$MODO" in
  completo)
    if [[ -z "${CPYD_EMAIL:-}" || -z "${CPYD_APIKEY:-}" ]]; then
      echo "MODO=completo requiere CPYD_EMAIL y CPYD_APIKEY (ver README). Para medir sin red: MODO=validacion" >&2
      exit 1
    fi
    FLAGS="" ;;
  validacion) FLAGS="--validate-only" ;;
  *) echo "MODO debe ser 'completo' o 'validacion'" >&2; exit 1 ;;
esac

mkdir -p "$SALIDA"
{
  echo "fecha: $(date -Iseconds)"
  echo "modo: $MODO"
  echo "entrada: $ENTRADA ($(wc -l < "$ENTRADA") líneas)"
  echo "repeticiones: $REPETICIONES"
  echo "nucleos (nproc): $(nproc 2>/dev/null || echo ?)"
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //' || echo ?)"
  echo "compilador: $(${CXX:-g++} --version 2>/dev/null | head -1)"
  echo "argumentos extra: ${EXTRA:-ninguno}"
} > "$SALIDA/entorno.txt"

METRICAS="$SALIDA/metricas_programa.csv"
rm -f "$METRICAS"
echo "hilos,repeticion,segundos_pared,codigo_salida" > "$SALIDA/corridas.csv"

for p in $HILOS; do
  for r in $(seq 1 "$REPETICIONES"); do
    out="$SALIDA/resumen_${p}h_r${r}.csv"
    ini=$(date +%s.%N)
    set +e
    # shellcheck disable=SC2086
    ./programa --input "$ENTRADA" --output "$out" --threads "$p" --metrics "$METRICAS" $FLAGS $EXTRA \
      > "$SALIDA/log_${p}h_r${r}.txt" 2>&1
    codigo=$?
    set -e
    fin=$(date +%s.%N)
    seg=$(awk -v a="$ini" -v b="$fin" 'BEGIN{printf "%.4f", b - a}')
    echo "$p,$r,$seg,$codigo" >> "$SALIDA/corridas.csv"
    printf 'hilos=%-2s rep=%s  %8.3f s  (código %s)\n' "$p" "$r" "$seg" "$codigo"
    [[ "$codigo" -eq 0 ]] || echo "  ¡atención! revise $SALIDA/log_${p}h_r${r}.txt" >&2
    sleep "$PAUSA"
  done
done

# Mediana por cantidad de hilos, speedup y eficiencia (solo corridas exitosas)
awk -F, 'NR > 1 && $4 == 0 { t[$1] = t[$1] " " $3; orden[$1] = orden[$1] ? orden[$1] : ++n; h[n] = $1 }
END {
  print "hilos,mediana_s,speedup,eficiencia"
  for (i = 1; i <= n; i++) {
    p = h[i]; k = split(t[p], v, " ")
    for (a = 1; a <= k; a++) for (b = a + 1; b <= k; b++) if (v[b] < v[a]) { x = v[a]; v[a] = v[b]; v[b] = x }
    med = (k % 2) ? v[(k + 1) / 2] : (v[k / 2] + v[k / 2 + 1]) / 2
    if (i == 1) { t1 = med; p1 = p }
    s = t1 / med
    printf "%s,%.4f,%.3f,%.3f\n", p, med, s, s / (p / p1)
  }
}' "$SALIDA/corridas.csv" > "$SALIDA/resumen.csv"

{
  echo "| Hilos (p) | Tiempo mediano [s] | Speedup T1/Tp | Eficiencia S/p |"
  echo "|---:|---:|---:|---:|"
  tail -n +2 "$SALIDA/resumen.csv" | awk -F, '{ printf "| %s | %s | %s | %s |\n", $1, $2, $3, $4 }'
} > "$SALIDA/resumen.md"

# En modo completo, todas las corridas deben producir exactamente los mismos archivos
if [[ "$MODO" == "completo" ]]; then
  for sufijo in "" ".errors.csv" ".tasas.csv"; do
    ref=""
    for p in $HILOS; do
      for r in $(seq 1 "$REPETICIONES"); do
        f="$SALIDA/resumen_${p}h_r${r}.csv${sufijo}"
        [[ -f "$f" ]] || continue
        if [[ -z "$ref" ]]; then ref="$f"
        elif ! cmp -s "$ref" "$f"; then
          echo "ADVERTENCIA: $f difiere de $ref (¿la API respondió distinto entre corridas?)" >&2
        fi
      done
    done
  done
  echo "Verificación: resúmenes, errores y tasas comparados entre todas las corridas."
fi

echo
cat "$SALIDA/resumen.md"
echo
echo "Tiempos por fase (columna t_tasas = consultas a la API): $METRICAS"
echo "Resultados guardados en $SALIDA/"
