# Tools

The command-line tools that ship with Neasant, and the libraries behind them. Each tool is a
thin `main` over the library's public headers and a library target of its own; a front end
that wants the same capability links that target and calls what the tool calls.

| Directory | Library | Command line | Page |
|---|---|---|---|
| [`render/`](render/README.md) | `neasant_render` | `nes_render` | [nes-machine.md](../docs/nes-machine.md#the-apu) |

## Contents

- [Building](#building)
- [See also](#see-also)

## Building

The tools build when `NEASANT_BUILD_TOOLS` is on, the default for a top-level build, and
whenever the tests are on (`NEASANT_BUILD_TESTS`), since the suite links the tools' libraries.
A parent build that vendors the library sets the first to have them without the suite. Each
tool is its own target:

```
cmake --build build --config Release --target nes_render
```

The binary lands in `build/tools/render/`, or in `build\tools\render\Release\` on Windows.
Keep `--config Release` there: a Visual Studio build is `Debug` without it.

`tools/` is on the public include path of `neasant_render`, so its header is included by
directory: `render/wav_writer.h`.

## See also

- [docs/README.md](../docs/README.md): the documentation index.
- [The project README](../README.md): what is built and what is not.
