#!/bin/bash
# Runs the load-test matrix (3 scenarios x sizes) one run after the other and collects the results.
#
# usage: run_matrix.sh <label> <gage-loadtest.exe> <outdir> [server-config.json|-] [sizes...]
#
#   label      names the files: <outdir>/<label>_<scenario>_<N>.log (the full output of each run)
#              and <outdir>/<label>_results.txt (the RESULT line of each run, in run order)
#   server-config.json   passed to the server as --server-config; "-" or omitted: built-in defaults
#   sizes      bot counts; default 50 200 500 1000
#
# Runs are sequential on purpose (one machine, one set of cores) with 3 s between runs; each is
# `--seconds 15 --warmup 5 --ramp 100` (see docs/perf/BASELINE.md). Prints "exit=<code>" after every
# run and "DONE <label>" at the end. Exit status: 0 when every run exited 0, 1 otherwise.
# Turn two result files into the markdown tables of docs/perf/RESULTS.md with
#   node apps/loadtest/results_tables.mjs <baseline_results.txt> <final_results.txt>
if [ $# -lt 3 ]; then
  echo "usage: run_matrix.sh <label> <gage-loadtest.exe> <outdir> [server-config.json|-] [sizes...]" >&2
  exit 2
fi
label="$1"; exe="$2"; out="$3"; cfg="${4:--}"
shift $(( $# < 4 ? $# : 4 ))
sizes=("$@")
if [ ${#sizes[@]} -eq 0 ]; then sizes=(50 200 500 1000); fi
if [ ! -x "$exe" ]; then echo "run_matrix: $exe is not an executable file" >&2; exit 2; fi
mkdir -p "$out"
: > "$out/${label}_results.txt"
failed=0
for n in "${sizes[@]}"; do
  for sc in spread mixed hotspot; do
    log="$out/${label}_${sc}_${n}.log"
    echo "=== $(date +%H:%M:%S) $label $sc $n"
    if [ "$cfg" != "-" ]; then
      "$exe" --bots "$n" --scenario "$sc" --seconds 15 --warmup 5 --ramp 100 --server-config "$cfg" > "$log" 2>&1
    else
      "$exe" --bots "$n" --scenario "$sc" --seconds 15 --warmup 5 --ramp 100 > "$log" 2>&1
    fi
    code=$?
    echo "exit=$code"
    if [ $code -ne 0 ]; then failed=1; fi
    grep "^RESULT" "$log" >> "$out/${label}_results.txt"
    sleep 3
  done
done
echo "DONE $label"
exit $failed
