# TERM47: working rules

- Work on a new branch; Victor creates and merges the PRs. Never force-push or rewrite history.
- Commit author: "Victor Valdes" <valdesvj@users.noreply.github.com>.
- The same base as Chess 47 and NAV47 (dmcp/, tools/pg5tool.py, test/sim): keep them in step.
- src/vt.c is plain C (no DMCP): a change to it needs `make test` and `make compare` (pyte) to pass.
- src/fonts.h is generated (make fonts, tools/mkfont.py); never edit it by hand.
- The link (src/proto.h) is shared by src/link.c and tools/term47host.py: change both together; the PC never has
  more than T47_WINDOW lines unread on the calculator (DMCP keeps 256 bytes a channel).
- DMCP5: 8 KB of static RAM. Big buffers go on the heap (the screens, the scrollback, the menus); check .bss in
  arm-none-eabi-size.
- Check every screen in the simulator (make sim; test/run_sim.py) before committing.
- Ask before anything that can't be undone. Pull requests only for term47.pg5.
