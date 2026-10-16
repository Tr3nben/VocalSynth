"""Static checks of .github/workflows/corpus-eval.yml (docs/CORPUS.md, "Workflow")."""

from __future__ import annotations

import os

import yaml

from conftest import REPO

PATH = os.path.join(REPO, ".github", "workflows", "corpus-eval.yml")


def load():
    with open(PATH, encoding="utf-8") as f:
        return yaml.safe_load(f)


def test_manual_trigger_only():
    wf = load()
    triggers = wf[True]  # YAML 1.1 reads the key "on" as True
    assert list(triggers) == ["workflow_dispatch"]
    assert set(triggers["workflow_dispatch"]["inputs"]) >= {"datasets", "sweep", "gates", "stickiness"}


def test_job_shape():
    job = load()["jobs"]["evaluate"]
    assert job["runs-on"] == "ubuntu-24.04"
    assert job["timeout-minutes"] <= 60
    steps = job["steps"]
    uses = [s.get("uses", "") for s in steps]
    assert any(u.startswith("actions/cache/restore@") for u in uses)
    assert any(u.startswith("actions/cache/save@") for u in uses)
    assert any(u.startswith("actions/upload-artifact@") for u in uses)
    runs = "\n".join(s.get("run", "") for s in steps)
    assert "--target vs-eval" in runs and "GITHUB_STEP_SUMMARY" in runs
    # inputs reach scripts only via env (no ${{ }} inside run scripts)
    assert "${{" not in runs
    # no audio in the artifact: it uploads the results directory only
    upload = next(s for s in steps if s.get("uses", "").startswith("actions/upload-artifact@"))
    assert upload["with"]["path"].endswith("/corpus/results")


def test_default_sweep_grid():
    inputs = load()[True]["workflow_dispatch"]["inputs"]
    assert inputs["gates"]["default"] == "-45,-40,-35"
    assert inputs["stickiness"]["default"] == "30,50,70"
