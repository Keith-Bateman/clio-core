#!/usr/bin/env bash
# Real end-to-end verification of the ported agent-interceptor + http_forward_shim.py,
# on a GPU compute node (NOT the login node) -- starts Ollama, dt_demo_server, and the
# shim, then runs a couple of real SWE-bench instances through the proxy instead of
# talking to Ollama directly, and checks the run behaves the same as a direct run.
#
# Prepared by a background fork per the approved plan (Phase A, minimal-shim decision).
# NOT submitted automatically -- review and `sbatch` this yourself:
#   sbatch /work/hdd/bekn/kbateman/clio-core/context-exploration-engine/agent-interceptor/demo/slurm_verify_proxy.sh
#
#SBATCH --job-name=verify-dt-proxy
#SBATCH --partition=gpuA100x4
#SBATCH --account=bekn-delta-gpu
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --gpus-per-node=1
#SBATCH --mem=60G
#SBATCH --time=01:00:00
#SBATCH --output=/work/hdd/bekn/kbateman/clio-core/context-exploration-engine/agent-interceptor/demo/logs/verify-%j.out
#SBATCH --error=/work/hdd/bekn/kbateman/clio-core/context-exploration-engine/agent-interceptor/demo/logs/verify-%j.err

set -uo pipefail
mkdir -p /work/hdd/bekn/kbateman/clio-core/context-exploration-engine/agent-interceptor/demo/logs

CLIO_CORE=/work/hdd/bekn/kbateman/clio-core
DEMO="$CLIO_CORE/context-exploration-engine/agent-interceptor/demo"
BIN="$CLIO_CORE/build/bin"
SWEBENCH_DIR=/work/hdd/bekn/kbateman/SWE-bench

PIDS=()
cleanup() {
  echo "[cleanup] killing: ${PIDS[*]:-none}"
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done
  sleep 2
  for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null; done
}
trap cleanup EXIT

# ── Ollama (same pattern as run_swebench.sh, job-specific port to avoid collisions) ──
source /work/hdd/bekn/kbateman/install/x86_64/ollama_env.sh 2>/dev/null || true
OLLAMA_PORT="$(( 20000 + SLURM_JOB_ID % 20000 ))"
export OLLAMA_HOST="127.0.0.1:${OLLAMA_PORT}"
export OLLAMA_NUM_GPU=4
ollama serve &
PIDS+=($!)
for i in $(seq 1 60); do curl -sf "http://${OLLAMA_HOST}/api/tags" >/dev/null 2>&1 && break; sleep 2; done
echo "[ollama] up at ${OLLAMA_HOST}"

MODEL="${SB_MODEL:-llama3.1:8b}"
if ! curl -sf "http://${OLLAMA_HOST}/api/tags" | grep -qF "\"${MODEL}\""; then
  OLLAMA_HOST="$OLLAMA_HOST" ollama pull "$MODEL"
fi

# ── dt_demo_server (proxy + interception + tracker + ctx_untangler pools) ──
export CLIO_SERVER_CONF="$DEMO/dt_provenance_server.yaml"
export LD_LIBRARY_PATH="$BIN:${LD_LIBRARY_PATH:-}"
# proxy_runtime.cc's SelectUpstream() reads OLLAMA_HOST as a full URL (http://host:port),
# distinct from the bare "host:port" form ollama serve itself expects above.
export OLLAMA_HOST="http://127.0.0.1:${OLLAMA_PORT}"
"$BIN/dt_demo_server" > "$DEMO/logs/dt_demo_server-${SLURM_JOB_ID}.log" 2>&1 &
PIDS+=($!)
for i in $(seq 1 30); do grep -q "All 8 pools created successfully" "$DEMO/logs/dt_demo_server-${SLURM_JOB_ID}.log" 2>/dev/null && break; sleep 1; done
echo "[dt_demo_server] up"

# ── HTTP forward shim (port 9090) ──
PYTHONPATH="$BIN" LD_LIBRARY_PATH="$BIN:${LD_LIBRARY_PATH:-}" \
  /u/kbateman/miniconda3/bin/python3 "$DEMO/http_forward_shim.py" --port 9090 --provider ollama \
  > "$DEMO/logs/shim-${SLURM_JOB_ID}.log" 2>&1 &
PIDS+=($!)
sleep 3
echo "[shim] up on :9090"

# ── Run 2 SWE-bench instances THROUGH the proxy instead of straight to Ollama ──
cd "$SWEBENCH_DIR"
VENV_PY="$SWEBENCH_DIR/.venv/bin/python"
RUN_ID="proxy-verify-${SLURM_JOB_ID}"
"$VENV_PY" agent/run_agent.py \
  --dataset princeton-nlp/SWE-bench_Lite --split test \
  --model "$MODEL" \
  --base-url "http://127.0.0.1:9090/_session/${RUN_ID}/v1" \
  --api-key ollama \
  --max-steps 40 --bash-timeout 60 \
  --run-id "$RUN_ID" \
  --max-instances 2

AGENT_EXIT=$?
echo "[agent] exit code: $AGENT_EXIT"

# ── Sanity: did InteractionRecords actually land? (dt_demo_server log line per forward) ──
echo "=== ForwardHttp lines in dt_demo_server log ==="
grep -c "ForwardHttp: session=${RUN_ID}" "$DEMO/logs/dt_demo_server-${SLURM_JOB_ID}.log" || true

echo "[done] Compare trajectories/${RUN_ID}/ against a direct (non-proxied) run of the"
echo "       same --model/--instance-ids to confirm the proxy is transparent (same"
echo "       tool-call sequence / same termination_reason per instance)."
exit "$AGENT_EXIT"
