# AI Assistance Policy

## How this project uses AI

This project was **developed with the help of AI** — Claude (Anthropic), Gemini (Google), and Grok (xAI) — as a cooperation. AI assisted with brainstorming, code generation, and documentation, but I reviewed all output, validated results against reference implementations, and take full responsibility for the work and any mistakes.

## Responsible use

- ✅ **I checked the results** — The terminal is checked against pyte, a reference terminal emulator: real programs (ls, top, less, vi, lynx, w3m, man) give the same screen, cell for cell, at all four sizes (`make compare`)
- ✅ **I maintained domain expertise** — I guide the AI with the design decisions: the DM42n's screen, keys and memory, the serial console of DMCP5 3.58, and how an old Unix terminal should feel on the calculator
- ✅ **I take full responsibility** — The work is mine; any errors are mine to fix
- ✅ **I'm transparent** — This file documents how AI was used

## Testing & Validation

- Unit tests: the VT102 escape sequences, UTF-8, the scrollback, the resize, and the link's escaping (`make test`)
- Comparison with pyte on real full-screen programs, at 66x26, 80x26, 50x16 and 40x10 (`make compare`)
- End to end in the PC simulator: the real program, the PC bridge and a real bash, with DMCP's 256-byte buffer
  simulated, so no line may be lost (`make sim`, `test/run_sim.py`)
- Every screen checked in the PC simulator (`test/sim/render.py`) before a commit
- To be tested on real DM42n hardware with DMCP5 3.58, on battery and on USB power

## License

This does not affect the GNU GPL-3.0 license. All code remains open-source and fully accessible.

---

*For more details, see `NOTICE` and the README.*
