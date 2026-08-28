# Contributing

Bug reports and focused pull requests are welcome. For input, output or session
crashes, include the GPU, distribution, wlroots/SceneFX versions and the end of
`~/.local/state/gluewc.log`.

Before submitting code:

```sh
make clean
make
git diff --check
```

Keep changes small and follow the surrounding dwl-style C. Avoid unrelated
formatting and generated protocol files. Runtime behavior that users may want
to tune should normally be exposed through `config.conf` or `config.def.h`.

The project targets wlroots 0.20 and SceneFX 0.5. Changes for another wlroots
release should be proposed separately because wlroots does not promise a stable
API between minor versions.
