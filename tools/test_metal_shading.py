"""The half precision pixel shaders' text (port/linux/src/nv2a_psh.c, display.half_precision_shading;
port/tvos/PERFORMANCE.md "Shading (A12)"): half_literals makes the combiners' decimal literals half (in Metal a float
literal turns a half expression float, and max(half, 0.0) does not compile) and leaves everything else alone; the
setting is off by default, so the shaders' text is as it was unless it is asked for."""

import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from harness import function, mutated  # noqa: E402

CASES = {
    "\t\thalf3 cC = (2.0 * max(ht0.rgb, 0.0) - 1.0);\n": "\t\thalf3 cC = (2.0h * max(ht0.rgb, 0.0h) - 1.0h);\n",
    "cAB = clamp((cAB * 4.0), -1.0, 1.0);": "cAB = clamp((cAB * 4.0h), -1.0h, 1.0h);",
    # names with digits and indices stay as they are
    "half4(ps_c0[3]).rgb + ht0.a + hr1.b + xT2.w": "half4(ps_c0[3]).rgb + ht0.a + hr1.b + xT2.w",
    "(int(hr0.a * 255.0 + 0.5) & 1)": "(int(hr0.a * 255.0h + 0.5h) & 1)",
    # an exponent's literal and one already typed are left alone
    "max(x, 1.0e-6) + 2.0h + 3.0f": "max(x, 1.0e-6) + 2.0h + 3.0f",
    "": "",
}

PROGRAM = r"""
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
struct xgpu_text { char *buffer; unsigned long length; unsigned long capacity; };
static void xgpu_text_append(struct xgpu_text *text, const char *format, ...)
{
	va_list arguments;
	int needed;
	va_start(arguments, format);
	needed = vsnprintf(NULL, 0, format, arguments);
	va_end(arguments);
	if (text->length + (unsigned long)needed + 1 > text->capacity)
	{
		text->capacity = (text->length + (unsigned long)needed + 1) * 2;
		text->buffer = realloc(text->buffer, text->capacity);
	}
	va_start(arguments, format);
	vsnprintf(text->buffer + text->length, text->capacity - text->length, format, arguments);
	va_end(arguments);
	text->length += (unsigned long)needed;
}
%(code)s
int main(int argc, char **argv)
{
	/* a prefix that must stay as it is (the float part of a shader) */
	struct xgpu_text text = { 0 };
	unsigned long start;
	xgpu_text_append(&text, "float f = 1.0;|");
	start = text.length;
	xgpu_text_append(&text, "%%s", argc > 1 ? argv[1] : "");
	half_literals(&text, start);
	fputs(text.buffer, stdout);
	return 0;
}
"""


def rewriter(tmp_path, fault=None):
    source = (ROOT / "port/linux/src/nv2a_psh.c").read_text()
    code = function(source, "digit") + "\n" + function(source, "half_literals")
    if fault:
        code = mutated(code, *fault)
    (tmp_path / "t.c").write_text(PROGRAM % {"code": code})
    executable = tmp_path / "t"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", str(tmp_path / "t.c"),
                    "-o", str(executable)], check=True)
    return executable


def rewrite(executable, text):
    output = subprocess.run([str(executable), text], capture_output=True, text=True, check=True).stdout
    prefix, rest = output.split("|", 1)
    assert prefix == "float f = 1.0;", "half_literals changed text before its start"
    return rest


@pytest.mark.parametrize("before", list(CASES))
def test_half_literals(tmp_path, before):
    assert rewrite(rewriter(tmp_path), before) == CASES[before]


def test_negative_control_names(tmp_path):
    # a rewriter that does not require a decimal point turns ps_c0[3]'s index
    # into 3h: the cases above must be able to see that
    executable = rewriter(tmp_path, ("memchr(first, '.', (size_t)(at - first + 1))", "1"))
    assert rewrite(executable, "half4(ps_c0[3])") != "half4(ps_c0[3])"


def test_default_text_unchanged():
    """the shading flags are 0 unless display.half_precision_shading (or a diagnostic) asks: the shaders the game
    has always built, and the shader manifests' texts, stay as they were"""
    source = (ROOT / "port/macos/guest/d3d8_metal.c").read_text()
    body = function(source, "shading_mask")
    assert re.search(r'mask = \(unsigned char\)\(config_boolean\("display.half_precision_shading"\) \? SHADING_FAST : 0\)',
                     body)
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    assert re.search(r'"display.half_precision_shading", _config_boolean, "false"', config)
