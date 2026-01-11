#!/usr/bin/env bash
# Bench Bedrock with a concise stdout report (no output files)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

CMD_DEFAULT="$ROOT_DIR/build/quartz --interp $ROOT_DIR/bedrock/hello_world.qz"
URL_DEFAULT='http://127.0.0.1:8080/'
DURATION_DEFAULT='30s'
CONCURRENCY_DEFAULT='200'
THREADS_DEFAULT='4'
WARMUP_DEFAULT='2s'
INTERVAL_DEFAULT='0.2'   # memory sampling interval seconds

CMD="$CMD_DEFAULT"
URL="$URL_DEFAULT"
DURATION="$DURATION_DEFAULT"
CONCURRENCY="$CONCURRENCY_DEFAULT"
THREADS="$THREADS_DEFAULT"
WARMUP="$WARMUP_DEFAULT"
INTERVAL="$INTERVAL_DEFAULT"
TIMEOUT_SECS="60"        # for curl readiness + hey per-request timeout
VERBOSE=0

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  --cmd "..."          Command to start the server (default: $CMD_DEFAULT)
  --url "..."          URL to benchmark (default: $URL_DEFAULT)
  --duration 30s       Benchmark duration (default: $DURATION_DEFAULT)
  --concurrency 200    Concurrent clients (default: $CONCURRENCY_DEFAULT)
  --threads 4          wrk threads (default: $THREADS_DEFAULT)
  --warmup 2s          Warmup duration (default: $WARMUP_DEFAULT)
  --interval 0.2       Memory sample interval seconds (default: $INTERVAL_DEFAULT)
  --timeout 60         Curl/hey timeout seconds (default: $TIMEOUT_SECS)
  --verbose            Print server stderr tail at end
  -h, --help           Show help

Examples:
  $0
  $0 --cmd "$ROOT_DIR/build/quartz --interp $ROOT_DIR/bedrock/hello_world.qz" --url "http://127.0.0.1:8080/health"
  $0 --cmd "python3 -m http.server 8080" --url "http://127.0.0.1:8080/"
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --cmd) CMD="$2"; shift 2;;
    --url) URL="$2"; shift 2;;
    --duration) DURATION="$2"; shift 2;;
    --concurrency) CONCURRENCY="$2"; shift 2;;
    --threads) THREADS="$2"; shift 2;;
    --warmup) WARMUP="$2"; shift 2;;
    --interval) INTERVAL="$2"; shift 2;;
    --timeout) TIMEOUT_SECS="$2"; shift 2;;
    --verbose) VERBOSE=1; shift 1;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown arg: $1"; usage; exit 2;;
  esac
done

log() { printf '%s\n' "$*"; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1
}

require_one_of() {
  local ok=1
  for c in "$@"; do
    if need_cmd "$c"; then ok=0; fi
  done
  return $ok
}

to_mib() {
  # KB -> MiB
  awk -v kb="${1:-0}" 'BEGIN{printf "%.1f", (kb/1024.0)}'
}

ss_count_state() {
  local state="$1"
  if ! need_cmd ss; then
    echo ""
    return 0
  fi
  ss -Htan "state" "$state" 2>/dev/null | wc -l | tr -d ' '
}

extract_first_number() {
  # Prints first floating/decimal number found in stdin line
  sed -nE 's/.*([0-9]+([.][0-9]+)?).*/\1/p' | head -n1
}

SERVER_PID=""
SERVER_STDOUT=""
SERVER_STDERR=""
TMPDIR=""
MEM_SAMPLER_PID=""
MAX_RSS_KB=0
MAX_HWM_KB=0
MAX_THREADS=0

cleanup() {
  set +e
  if [[ -n "${MEM_SAMPLER_PID}" ]]; then
    kill "${MEM_SAMPLER_PID}" 2>/dev/null || true
  fi
  if [[ -n "${SERVER_PID}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    sleep 0.5
    kill -9 "$SERVER_PID" 2>/dev/null || true
  fi
  if [[ -n "${TMPDIR}" ]] && [[ -d "${TMPDIR}" ]]; then
    rm -rf "$TMPDIR" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

TMPDIR="$(mktemp -d -t bench_bedrock.XXXXXX)"
SERVER_STDOUT="$TMPDIR/server.stdout"
SERVER_STDERR="$TMPDIR/server.stderr"
MEM_CSV="$TMPDIR/memory_samples.csv"
echo "t_epoch,vmrss_kb,vmhwm_kb,threads" > "$MEM_CSV"

log "=== Bedrock Benchmark ==="
log "cmd: $CMD"
log "url: $URL"
log "duration: $DURATION | concurrency: $CONCURRENCY | wrk_threads: $THREADS"
log "warmup: $WARMUP | mem_interval: ${INTERVAL}s | timeout: ${TIMEOUT_SECS}s"
log ""

OS_LINE="$(uname -srmo 2>/dev/null || uname -a)"
CPU_LINE="$(LC_ALL=C lscpu 2>/dev/null | awk -F: '/Model name/ {gsub(/^ +/,"",$2); print $2; exit}')"
CPU_CORES="$(LC_ALL=C lscpu 2>/dev/null | awk -F: '/^CPU\(s\)/ {gsub(/^ +/,"",$2); print $2; exit}')"
MEM_TOTAL="$(free -h 2>/dev/null | awk '/^Mem:/ {print $2; exit}')"
NOFILE_LIMIT="$(ulimit -n 2>/dev/null || echo "")"
SOMAXCONN="$(sysctl -n net.core.somaxconn 2>/dev/null || echo "")"
PORT_RANGE="$(sysctl -n net.ipv4.ip_local_port_range 2>/dev/null || echo "")"

log "System: $OS_LINE"
[[ -n "$CPU_LINE" ]] && log "CPU: $CPU_LINE (${CPU_CORES:-?} cores)"
[[ -n "$MEM_TOTAL" ]] && log "RAM: $MEM_TOTAL"
[[ -n "$NOFILE_LIMIT" ]] && log "ulimit -n: $NOFILE_LIMIT"
[[ -n "$SOMAXCONN" ]] && log "net.core.somaxconn: $SOMAXCONN"
[[ -n "$PORT_RANGE" ]] && log "ip_local_port_range: $PORT_RANGE"
log ""

if curl -fsS --max-time 0.5 "$URL" >/dev/null 2>&1; then
  log "warning: $URL is already responding before start"
  log "         stop the existing server or change --url/port for trustworthy results"
  log ""
fi

# --- Start server ---
log "Starting server..."
bash -lc "$CMD" >"$SERVER_STDOUT" 2>"$SERVER_STDERR" &
SERVER_PID=$!
log "pid: $SERVER_PID"

# --- Wait until URL is reachable ---
log "Waiting for readiness..."
ready=0
for i in $(seq 1 200); do
  if curl -fsS --max-time 1 "$URL" >/dev/null 2>&1; then
    # Only accept readiness if the server process we started is still alive.
    if kill -0 "$SERVER_PID" 2>/dev/null; then
      ready=1
      break
    fi
  fi
  # If server died, fail early
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    log "Server exited early. stderr tail:"
        tail -n 80 "$SERVER_STDERR" 2>/dev/null || true
    exit 1
  fi
  sleep 0.1
done

if [[ "$ready" -ne 1 ]]; then
  log "Server did not become ready in time. Try increasing --timeout or check logs."
  exit 1
fi

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  log "Server process is not running after readiness wait. stderr tail:"
  tail -n 80 "$SERVER_STDERR" 2>/dev/null || true
  exit 1
fi

log "Ready."

ESTAB_BEFORE="$(ss_count_state established)"
TW_BEFORE="$(ss_count_state time-wait)"

# --- Memory sampler (writes samples to a temp CSV) ---
(
  while kill -0 "$SERVER_PID" 2>/dev/null; do
    t="$(date +%s.%N)"
    rss_kb="$(awk '/VmRSS:/ {print $2}' "/proc/$SERVER_PID/status" 2>/dev/null || echo "")"
    hwm_kb="$(awk '/VmHWM:/ {print $2}' "/proc/$SERVER_PID/status" 2>/dev/null || echo "")"
    thr="$(awk '/Threads:/ {print $2}' "/proc/$SERVER_PID/status" 2>/dev/null || echo "")"
    printf '%s,%s,%s,%s\n' "$t" "${rss_kb:-}" "${hwm_kb:-}" "${thr:-}" >> "$MEM_CSV" || true
    sleep "$INTERVAL"
  done
) &
MEM_SAMPLER_PID=$!

# --- Warmup ---
log "Warmup for $WARMUP..."
if need_cmd wrk; then
  wrk -t1 -c"$CONCURRENCY" -d"$WARMUP" "$URL" >/dev/null 2>&1 || true
else
  # simple curl warmup loop
  end=$((SECONDS + 2))
  while [[ $SECONDS -lt $end ]]; do curl -fsS --max-time 1 "$URL" >/dev/null 2>&1 || true; done
fi

# --- Benchmark ---
log "Benchmarking..."

WRK_OUT=""
HEY_OUT=""

if need_cmd wrk; then
  WRK_OUT="$(wrk -t"$THREADS" -c"$CONCURRENCY" -d"$DURATION" --latency "$URL" 2>&1 || true)"
else
  log "note: wrk not found (install: sudo apt-get install -y wrk)"
fi

if need_cmd hey; then
  HEY_OUT="$(hey -z "$DURATION" -c "$CONCURRENCY" -t "$TIMEOUT_SECS" "$URL" 2>&1 || true)"
else
  log "note: hey not found (install: sudo apt-get install -y hey)"
fi

ESTAB_AFTER="$(ss_count_state established)"
TW_AFTER="$(ss_count_state time-wait)"

# Stop sampler now that benchmark is over
kill "$MEM_SAMPLER_PID" 2>/dev/null || true
MEM_SAMPLER_PID=""

# Compute peaks from sampler CSV
if [[ -f "$MEM_CSV" ]]; then
  MAX_RSS_KB="$(awk -F',' 'NR>1 && $2 ~ /^[0-9]+$/ {if($2>m)m=$2} END{print m+0}' "$MEM_CSV")"
  MAX_HWM_KB="$(awk -F',' 'NR>1 && $3 ~ /^[0-9]+$/ {if($3>m)m=$3} END{print m+0}' "$MEM_CSV")"
  MAX_THREADS="$(awk -F',' 'NR>1 && $4 ~ /^[0-9]+$/ {if($4>m)m=$4} END{print m+0}' "$MEM_CSV")"
fi

# --- Summaries ---
log ""
log "=== Summary ==="

if [[ -n "$WRK_OUT" ]]; then
  wrk_rps="$(printf "%s\n" "$WRK_OUT" | awk '/Requests\/sec:/ {print $2; exit}')"
  wrk_p50="$(printf "%s\n" "$WRK_OUT" | awk '/^ *50%/ {print $2; exit}')"
  wrk_p90="$(printf "%s\n" "$WRK_OUT" | awk '/^ *90%/ {print $2; exit}')"
  wrk_p99="$(printf "%s\n" "$WRK_OUT" | awk '/^ *99%/ {print $2; exit}')"
  wrk_sock="$(printf "%s\n" "$WRK_OUT" | awk '/Socket errors:/ {sub(/^.*Socket errors: /, ""); print; exit}')"

  log "wrk:  rps=${wrk_rps:-?} | p50=${wrk_p50:-?} p90=${wrk_p90:-?} p99=${wrk_p99:-?}"
  [[ -n "$wrk_sock" ]] && log "wrk:  socket_errors: $wrk_sock"
else
  log "wrk:  (not run)"
fi

if [[ -n "$HEY_OUT" ]]; then
  hey_rps="$(printf "%s\n" "$HEY_OUT" | awk '/Requests\/sec:/ {print $2; exit}')"
  hey_avg="$(printf "%s\n" "$HEY_OUT" | awk '/Average:/ {print $2 " " $3; exit}')"
  hey_p50="$(printf "%s\n" "$HEY_OUT" | awk '/^ *50% in/ {print $3 " " $4; exit}')"
  hey_p90="$(printf "%s\n" "$HEY_OUT" | awk '/^ *90% in/ {print $3 " " $4; exit}')"
  hey_p99="$(printf "%s\n" "$HEY_OUT" | awk '/^ *99% in/ {print $3 " " $4; exit}')"
  hey_200="$(printf "%s\n" "$HEY_OUT" | awk '/\[200\]/{print $2; exit}')"
  hey_err="$(printf "%s\n" "$HEY_OUT" | awk '/^  \[[0-9]+\] Get / {gsub(/[\[\]]/,"",$1); print $1; exit}')"
  hey_err=${hey_err:-0}

  log "hey:  rps=${hey_rps:-?} | avg=${hey_avg:-?} | p50=${hey_p50:-?} p90=${hey_p90:-?} p99=${hey_p99:-?}"
  log "hey:  status_200=${hey_200:-?} | errors=${hey_err}"
else
  log "hey:  (not run)"
fi

log "mem:  peak_rss=$(to_mib "$MAX_RSS_KB") MiB | peak_hwm=$(to_mib "$MAX_HWM_KB") MiB | max_threads=${MAX_THREADS}"
if [[ -n "$ESTAB_BEFORE" ]] || [[ -n "$ESTAB_AFTER" ]]; then
  log "net:  established ${ESTAB_BEFORE:-?} -> ${ESTAB_AFTER:-?} | time_wait ${TW_BEFORE:-?} -> ${TW_AFTER:-?}"
fi

if (( VERBOSE == 1 )); then
  log ""
  log "=== Server stderr (tail) ==="
  tail -n 80 "$SERVER_STDERR" 2>/dev/null || true
fi

log ""
log "Tip: For fair comparisons vs JVM/Python, ensure 0 errors and compare p99 + peak RSS." 