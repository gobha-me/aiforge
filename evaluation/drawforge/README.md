# DrawForge evaluation consumer

This directory is a noninstalled, opt-in consumer for DrawForge protocol-v2
evaluation runs. It deliberately does not implement the production integration
tracked by issue 109 and introduces no DrawForge dependency into AIForge's
public package.

Build it with `-Daiforge_DRAWFORGE_EVALUATION=ON`. Prepare each run with the
released DrawForge `evaluation/tools/evaluate_v2.py prepare-run` command, then
execute:

```text
aiforge_drawforge_evaluation --run RUN_DIRECTORY \
  --matrix-root MATRIX_DIRECTORY --drawforge DRAWFORGE_BINARY
```

The runner reads the frozen model and sampling identity from `run.json`. It
allows only the bundled bridge executable through AIForge's bounded process
tool, requires provider-reported USD cost, and refuses a new inference when a
started run lacks cost evidence or the matrix's conservative spend reaches the
USD 3 ceiling. Provider-reported usage and cost are persisted before the runner
checks whether the model produced an accepted submission, so an unsuccessful
paid run remains accounted and does not wedge the rest of the matrix. A
matrix-wide lock prevents concurrent runners from racing that accounting
check; an interrupted process can leave the empty
`.aiforge-drawforge-eval.lock` directory for the operator to inspect and remove.

The bridge accepts only `tool.py submit PAYLOAD`. It reconstructs semantic
state by replaying immutable source frames plus committed requests through the
released DrawForge CLI. Direct-SVG attempts and semantic apply attempts share
the corpus limits of three submissions and twelve tool interactions.
