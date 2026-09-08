# forgectrl

The machine-services daemon for
[ForgeFIRM](https://github.com/openglow-org/forgefirm)-powered Glowforge
lasers.

It runs on the factory i.MX6 control board and serves HTTPS on port 443, with
the read-only routes on HTTP port 80. Motion is executed by exactly one of two
controllers,
[grblHAL-glowforge](https://github.com/openglow-org/grblHAL-glowforge) in GRBL
mode or the `gfcloud` web-service client in factory cloud mode, and forgectrl
owns everything around them: the controller supervisor, the pulse-device
broker, the motion-liveness gate, the cooling engine, the cameras, telemetry,
settings, diagnostics, the logging tree, the web control panel, the first-run
commissioning, and the A/B update system.

## Documentation

Everything is on **<https://docs.forgefirm.org/>**, which is the source of
truth for this project. This README is an index card.

| Subject | Page |
|---|---|
| The machine-services contract: the switch map, the safety readbacks, telemetry, mode supervision, pulse-device ownership, the hardware ownership table | [forgectrl](https://docs.forgefirm.org/technical/forgefirm/forgectrl/) |
| The cooling engine: the gates, flow verification, the job-state reports, the verdict file | [Cooling engine](https://docs.forgefirm.org/technical/forgefirm/cooling-engine/) |
| The cameras: the privacy gate, the encoders, the demosaic, the sensor profile | [Video pipeline](https://docs.forgefirm.org/technical/forgefirm/video-pipeline/) |
| Logging: the emitters, the tree, the levels, the export sanitizer | [Logging](https://docs.forgefirm.org/technical/forgefirm/logging/) |
| The update manager | [Install and update](https://docs.forgefirm.org/technical/forgefirm/install-and-update/) |
| The control panel, as an operator meets it | [The control panel](https://docs.forgefirm.org/usage/control-panel/) |
| Every settings key | [Settings](https://docs.forgefirm.org/usage/settings/) |
| Building it, the environment variables, and working on the panel | [Build](https://docs.forgefirm.org/developers/building/) |

## Build and test

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Werror
cmake --build build
```

Then run the test binaries under `build/`. A warning is a failure. The host
tests live in `tests/`; `tests/fflog_e2e.sh` proves the whole logging path
against a private rsyslogd.

It builds with CMake and links against ulfius, libjpeg and zlib. The target
needs Linux with imx-media, the coda VPU driver, and v4l-utils (`media-ctl`
and `v4l2-ctl`). The `meta-forgefirm` layer in the `forgefirm` repository
carries the recipe, which also installs the sysvinit script from `init/`.

For the board binary, cross-compile with
`forgefirm/scripts/bench/build-forgectrl.sh`. Never use a native host build as
proof.

### The control panel

The panel is a plain static page under `src/ui/`, bundled into the daemon at
build time. `tools/devserver.py` serves it against a real machine or a
built-in mock, with live reload:

```sh
cp .env.example .env            # then fill in GF_HOST / GF_TOKEN
python3 tools/devserver.py      # http://127.0.0.1:8081
python3 tools/devserver.py --mock
```

`.devcontainer/` packages the same thing for VS Code.
[Build](https://docs.forgefirm.org/developers/building/) describes both.

## Contributing

[AGENTS.md](AGENTS.md) carries the rules for this repository and for the
project: safety ordering, proof before done, the push order, and the writing
rules. They apply to human contributors too.

## License

MIT. See [LICENSE](LICENSE).
