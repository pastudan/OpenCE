"""display.half_res_convolution's targets (rasterizer_xbox_screen_effect.c): each pass of a convolution reads what the
pass before it drew, the first reads the screen at full resolution, the last draws into the screen, and the passes
between are all at half resolution."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["chain", "all-between-half"]
FUNCTION = "rasterizer_screen_effect_half_target"

# a fault in the function, and the case that must catch it
NEGATIVE_CONTROLS = {
    "copy-to-1": (("\t\treturn 0;", "\t\treturn 1;"), "chain"),
    "last-pass-half": (("pass == pass_count - 1 : pass == 0", "pass == pass_count : pass == 0"), "chain"),
    "first-pass-half": (("pass == pass_count - 1 : pass == 0", "pass == pass_count - 1 : pass < 0"), "chain"),
    "screen-kept": (("\treturn 1;\n}", "\treturn NONE;\n}"), "all-between-half"),
}


def generated(fault=None):
    code = function(read("source/rasterizer/xbox/rasterizer_xbox_screen_effect.c"), FUNCTION)
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("screen_effect_half_targets", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("screen_effect_half_targets", generated(fault)), case)
    assert status == CHECK_FAILED, f"the targets with a fault ({control}) passed '{case}': the test cannot see it"
