#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
bash "$repo_root/tools/test-capture-process-isolation-evidence-v2.sh"

fixture=$(mktemp -d)
cleanup_fixture() {
  find "$fixture" -depth -delete
}
trap cleanup_fixture EXIT
mkdir "$fixture/tools"
cp "$repo_root/tools/capture-process-isolation-evidence-v3.sh" \
  "$fixture/tools/capture-process-isolation-evidence-v3.sh"

cat >"$fixture/tools/capture-process-isolation-evidence-v2.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$#" "$@" >"$FAKE_LOG"
exit "${FAKE_STATUS-0}"
EOF

FAKE_LOG="$fixture/log" FAKE_STATUS=7 \
  bash "$fixture/tools/capture-process-isolation-evidence-v3.sh" \
  /absolute/evaluator aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
  /absolute/report.json >/dev/null 2>&1 && observed=0 || observed=$?
mapfile -t forwarded <"$fixture/log"
if ((observed != 7)) || [[ ${#forwarded[@]} -ne 4 ]] ||
  [[ ${forwarded[0]} != 3 ]] || [[ ${forwarded[1]} != /absolute/evaluator ]] ||
  [[ ${forwarded[2]} != aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ]] ||
  [[ ${forwarded[3]} != /absolute/report.json ]]; then
  echo "error: v3 capture did not preserve the bounded v2 lifecycle contract" >&2
  exit 1
fi

echo "process-isolation v3 capture delegation tests passed"
