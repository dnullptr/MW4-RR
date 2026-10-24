# CPU regression tests

After building, run `test.cmd "path\to\NGX\include"` from the project root.
The tests use fake COM objects and mock NGX functions. They do not create a
GPU device, launch the game, or validate visual results.

The runtime-shader test always checks rejection of missing shader input.
For an optional CPU-only byte-equivalence check, pass a privately supplied
supported native shader, its independently prepared expected guide shader,
and the full path to the pinned DXC DLL to `build/tests/runtime-shader.exe`.
This also checks concurrent cache reuse, altered-input rejection, and compiler
failure handling. These private fixture files are not distributed.

Coverage includes default settings, variable resource extents, configuration
validation, preset selection and retirement, unfinished command recordings,
fence completion, exactly-once hook forwarding, private transition lock
ordering, native Super Resolution replacement and history recovery, and
spatial fallback coordinates. The private-transition test includes an optional
`RR_RELEASE_NEGATIVE_CONTROL` compile definition that deliberately bypasses
the internal-call guard and is expected to fail. Never use that definition
for an addon build.
