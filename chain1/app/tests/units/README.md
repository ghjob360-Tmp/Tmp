# Test units

Built from the logue SDK's own examples (https://github.com/korginc/logue-sdk, commit e78b16c, BSD 3-Clause,
Copyright (c) 2018-2023 KORG INC.) with `tools/logue_test/build_units.sh`, then packed for CHAIN-1 with
`tools/logue/logue_pack.py`:

| File | Unit | SDK |
| --- | --- | --- |
| waves-mk1.lgu | waves (oscillator) | NTS-1 (nutekt-digital) |
| dummy-delfx-mk1.lgu | dummy-delfx (delay template: passes the sound through) | NTS-1 |
| pluck-mk2.lgu | pluck (oscillator) | NTS-1 mkII |
| dummy-modfx-mk2.lgu | dummy-modfx (modulation template; asks for 128 KB, more than the MOD place has) | NTS-1 mkII |

They are machine code for the NTS-1's ARM CPU, run on the FM-1 by CHAIN-1's interpreter (app/logue/).
