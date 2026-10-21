"""Compile the real draw-local feedback selection with fake texture objects.
This verifies copy deduplication only, not Metal resource synchronization."""
from pathlib import Path
import subprocess
import pytest

ROOT = Path(__file__).resolve().parent.parent


@pytest.mark.parametrize('matches', [1, 2, 4])
def test_one_snapshot_per_draw_not_across_draws(tmp_path, matches):
    source = (ROOT / 'port/macos/host/host_metal.m').read_text()
    start = source.index('\t\tfor (index = 0; index < METAL_STAGES; index++)', source.index('void host_metal_draw('))
    end = source.index('\n\t\t\tif (!textures[index]', start)
    loop = source[start:end] + '\n}\n'
    harness = r'''
#include <assert.h>
#include <stddef.h>
#define METAL_STAGES 4
#define YES 1
#define NO 0
struct texture { int pixel; } objects[8], scratch;
struct { unsigned color; } pass;
struct draw { unsigned textures[4]; };
static unsigned copies;
static struct texture *texture_object(unsigned h) { return h ? &objects[h] : NULL; }
static struct texture *feedback_copy(struct texture *t) { ++copies; scratch.pixel = t->pixel; return &scratch; }
static void select_textures(struct draw *draw) {
    unsigned index;
    struct texture *textures[4], *feedback_snapshot = NULL;
    int feedback_taken = NO;
'''
    harness += loop
    harness += r'''
    for (index = 0; index < METAL_STAGES; index++) {
        if (draw->textures[index] == pass.color) assert(textures[index] == &scratch && textures[index]->pixel == objects[1].pixel);
        else assert(textures[index] == texture_object(draw->textures[index]));
    }
}
int main(void) {
    struct draw draw = {{2,2,2,2}};
    pass.color = 1; objects[1].pixel = 17;
'''
    harness += f'    for (unsigned i = 0; i < {matches}; i++) draw.textures[i] = 1;\n'
    harness += r'''
    select_textures(&draw); assert(copies == 1);
    objects[1].pixel = 42;
    select_textures(&draw); assert(copies == 2 && scratch.pixel == 42);
    for (unsigned i = 0; i < 4; i++) draw.textures[i] = 2;
    select_textures(&draw); assert(copies == 2);
}
'''
    code = tmp_path / 'feedback.c'; code.write_text(harness)
    executable = tmp_path / 'feedback'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
