#!/usr/bin/env python3
"""run_sim.py - TERM47 end to end on the PC: the simulator (build/sim, the real main.c, vt.c, link.c) and
tools/term47host.py joined by a pseudo-terminal pair, as the DM42n and the PC by the USB cable, a real shell behind.

    make sim && python3 test/run_sim.py [--cmd "bash --norc -i"] [--png docs/] KEY KEY ... SNAP:name ...

The keys are the simulator's (test/sim/sim.c: TYPE:text, WAIT:ms, SNAP:name). It runs in build/simrun (a fresh
calculator disk: TERM47.CFG and MENU.TXT made again) and turns each SNAP into a PNG beside it (test/sim/render.py).
Lines lost in the simulated DMCP buffer are reported (T47_SIM_LOST): there must be none.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cmd', default='bash --norc --noprofile -i')
    ap.add_argument('--png', default=None, help='also copy the PNGs here')
    ap.add_argument('--keep', action='store_true', help='keep the calculator disk (settings) from the last run')
    ap.add_argument('--log', default=None)
    ap.add_argument('keys', nargs='*')
    a = ap.parse_args()
    run = os.path.join(ROOT, 'build', 'simrun')
    if not a.keep:
        shutil.rmtree(run, ignore_errors=True)
    os.makedirs(run, exist_ok=True)
    master, slave = os.openpty()
    env = dict(os.environ, PS1='victorio@arch:\\w$ ', HOME=os.environ.get('HOME', '/root'),
               USER='victorio', HISTFILE='/dev/null')
    host = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'term47host.py'), '--fd', str(master),
                             '--cmd', a.cmd] + (['--log', a.log] if a.log else []),
                            pass_fds=[master], env=env, cwd=os.path.expanduser('~'))
    time.sleep(0.3)
    senv = dict(os.environ, T47_PORT=os.ttyname(slave))
    p = subprocess.run([os.path.join(ROOT, 'build', 'sim')] + a.keys, cwd=run, env=senv,
                       stderr=subprocess.PIPE, text=True)
    host.terminate()
    host.wait()
    err = p.stderr
    sys.stderr.write(err)
    for ln in err.splitlines():
        if ln.startswith('snap '):
            name = ln.split()[1]
            png = os.path.join(run, name + '.png')
            subprocess.run([sys.executable, os.path.join(HERE, 'sim', 'render.py'), os.path.join(run, name), png],
                           check=True)
            if a.png:
                os.makedirs(a.png, exist_ok=True)
                shutil.copy(png, a.png)
    lost = 'T47_SIM_LOST' in err
    if lost:
        print('FAIL: lines lost in the DMCP buffer')
    return 1 if lost or p.returncode not in (0,) else 0


if __name__ == '__main__':
    sys.exit(main())
