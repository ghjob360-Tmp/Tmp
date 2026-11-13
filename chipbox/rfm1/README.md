# Chipbox as an .rfm1 (RemoteFM1)

The glue that builds Chipbox (Synthwave edition) with [FM1VST](https://github.com/Quixotic7/FM1VST) into an `.rfm1`
for [RemoteFM1](https://github.com/Quixotic7/RemoteFM1) (remotefm1.com, the iOS and Android apps).

- `core_chipbox.c`: the core. It uses FM1VST's Dinghy family glue (`core-glue/dinghy/`). Chipbox's main loop returns
  after each pass when built with `CHIPBOX_STEP`, so the glue's coroutine is just a call a tick. That is what lets a
  Dinghy app link in WebAssembly.
- `core_chipbox_desc.c`, `core_chipbox.cmake`: the descriptor and the build (`fm1_add_core`, `fm1_add_rfm1`).
- `golden_chipbox.txt`: the golden check. The same script runs on Chipbox's own host sim and on the core module,
  and the screens must be identical. It passed: 0 of 57600 pixels differ.
- `build_rfm1.sh`: clones FM1VST, links this folder in and builds, checks and runs the `.rfm1`.

Not in the .rfm1: Bluetooth (Brothers Link), USB audio and the installer. Projects, patches and settings live in
RemoteFM1's flash image, in the FM-1's own layout.
