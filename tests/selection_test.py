"""Exercise actual main selection linked to a matrix double that traps GPIO init."""
import json
import pathlib
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())

def run(*args):
    return subprocess.run([binary, *args], text=True, capture_output=True, timeout=10)

assert "TEST_GPIO_INITIALIZATION" in run().stderr
assert run("--help").returncode == 0
assert run("--output=colorlight", "--help").returncode == 0
with tempfile.TemporaryDirectory() as directory:
    config_path = pathlib.Path(directory) / "wall.json"
    config_path.write_text(json.dumps({
        "width": 1, "height": 1,
        "output": {"type": "colorlight", "interface": "ndpi-no-such", "firmware_profile": "legacy",
                   "receivers": [{"index": 0, "x": 0, "y": 0, "width": 1, "height": 1}]}}))
    for args in [(f"--config={config_path}",), ("--output=colorlight", f"--config={config_path}")]:
        result = run(*args)
        assert result.returncode != 0
        assert "TEST_GPIO_INITIALIZATION" not in result.stderr, result.stderr
        assert "interface" in result.stderr or "Linux" in result.stderr, result.stderr
    result = run("--output=hub75", f"--config={config_path}")
    assert result.returncode != 0 and "conflicts" in result.stderr
    assert "TEST_GPIO_INITIALIZATION" not in result.stderr
    result = run(f"--config={config_path}", "--led-rows=64")
    assert result.returncode != 0 and "Unexpected option" in result.stderr
    assert "TEST_GPIO_INITIALIZATION" not in result.stderr
result = run("--output=typo")
assert result.returncode != 0 and "must be hub75 or colorlight" in result.stderr
print("output selection tests passed (Colorlight does not initialize GPIO)")
