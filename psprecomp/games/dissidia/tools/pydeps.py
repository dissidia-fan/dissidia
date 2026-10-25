#!/usr/bin/env python3
"""numpy, Pillow and scipy for the Dissidia tools, at the versions in requirements.txt, on any OS.

    python3 games/dissidia/tools/pydeps.py      # set the environment up (or check it); prints its python

A tool that needs them asks for them when it runs as a script, before it imports them:

    if __name__ == '__main__':
        import pydeps
        pydeps.require()

If this Python already has exactly those versions, require() returns. Otherwise the tool starts again with a
private environment, tools/.venv/<os>-<cpu>/, made the first time (python -m venv + pip install, one download of
about 60 MB; on Debian/Ubuntu `python3 -m venv` needs the python3-venv package). The versions are pinned because
builds write into work/mods, which is tracked: the same versions give the same bytes on every machine. pip
installs only files whose sha256 is listed in requirements.txt (--require-hashes; tools/pin_hashes.py writes them).

DISSIDIA_VENV=0: never make or use the environment; the tool runs with this Python as it is (any versions, so a
build may write bytes that differ from the tracked ones). Standard library only.
"""
import glob
import os
import platform
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REQUIREMENTS = os.path.join(HERE, 'requirements.txt')
MIN_PYTHON = (3, 11)                  # numpy 2.3, scipy 1.16
GUARD = 'DISSIDIA_PYDEPS'             # set for the restarted tool, which must not restart again


def log(msg):
    print('[pydeps] ' + msg, file=sys.stderr, flush=True)


def pins():
    """[(distribution, version)] from the name==version lines of requirements.txt."""
    out = []
    with open(REQUIREMENTS) as f:
        for line in f:
            m = re.match(r'\s*([A-Za-z0-9_.\-]+)==([^\s\\;#]+)', line.split('#', 1)[0])    # "name==version \" (then --hash lines)
            if m:
                out.append((m.group(1), m.group(2)))
    return out


def missing():
    """The pins this Python does not have: [(distribution, wanted version, installed version or None)]."""
    from importlib import metadata
    bad = []
    for name, ver in pins():
        try:
            have = metadata.version(name)
        except metadata.PackageNotFoundError:
            have = None
        if have != ver:
            bad.append((name, ver, have))
    return bad


def describe(bad):
    return ', '.join('%s %s (%s)' % (n, v, 'this Python has %s' % h if h else 'missing') for n, v, h in bad)


def venv_dir():
    """tools/.venv/<os>-<cpu>: one environment per platform, so a checkout shared by two machines (a Mac and a
    Linux VM, ...) never runs the other one's binaries."""
    return os.path.join(HERE, '.venv', '%s-%s' % (sys.platform, platform.machine().lower() or 'cpu'))


def venv_python(d=None):
    d = d or venv_dir()
    return os.path.join(d, 'Scripts', 'python.exe') if os.name == 'nt' else os.path.join(d, 'bin', 'python')


def ready(d=None):
    """True when the environment's Python runs and has every pin (it checks itself: pydeps.py --ok)."""
    py = venv_python(d)
    if not os.path.isfile(py):
        return False
    try:
        return subprocess.run([py, '-I', os.path.abspath(__file__), '--ok'], capture_output=True,
                              timeout=60).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def _version(py):
    if os.path.realpath(py) == os.path.realpath(sys.executable):
        return tuple(sys.version_info[:2])
    try:
        r = subprocess.run([py, '-c', 'import sys; print(*sys.version_info[:2])'], capture_output=True, text=True,
                           timeout=20)
        return tuple(int(x) for x in r.stdout.split()) if r.returncode == 0 else (0, 0)
    except (OSError, ValueError, subprocess.SubprocessError):
        return (0, 0)


def base_python():
    """A Python 3.11+ to make the environment with: this one, else one on PATH or in the usual install places
    (Blender started from the Dock does not see the shell's PATH). Blender's own Python comes last: its path
    changes with every Blender update, and the environment would have to be made again."""
    cands = [sys.executable] + [shutil.which(n) for n in ('python3.13', 'python3.12', 'python3.11', 'python3',
                                                          'python')]
    cands += ['/opt/homebrew/bin/python3', '/usr/local/bin/python3']
    cands += sorted(glob.glob('/Library/Frameworks/Python.framework/Versions/3.*/bin/python3'), reverse=True)
    seen, blender = set(), []
    for c in cands:
        if not c or not os.path.isfile(c) or os.path.realpath(c) in seen:
            continue
        seen.add(os.path.realpath(c))
        if 'blender' in c.lower():
            blender.append(c)
        elif _version(c) >= MIN_PYTHON:
            return c
    for c in blender:
        if _version(c) >= MIN_PYTHON:
            return c
    raise SystemExit('[pydeps] setting up numpy / Pillow / scipy needs Python %d.%d or newer; none was found'
                     % MIN_PYTHON)


def _stderr():
    try:
        return sys.stderr.fileno()
    except (AttributeError, OSError, ValueError):
        return None


def _lock(d):
    """A lock folder next to the environment (os.mkdir is atomic everywhere): one setup at a time."""
    lock = d + '.lock'
    os.makedirs(os.path.dirname(lock), exist_ok=True)
    t0, said = time.time(), False
    while True:
        try:
            os.mkdir(lock)
            return lock
        except FileExistsError:
            pass
        try:
            if time.time() - os.path.getmtime(lock) > 1800:        # left behind by a setup that was killed
                os.rmdir(lock)
                continue
        except OSError:
            continue
        if time.time() - t0 > 1800:
            raise SystemExit('[pydeps] %s is still locked; remove that folder if no setup is running' % lock)
        if not said:
            log('waiting for another setup to finish (%s)' % lock)
            said = True
        time.sleep(1)


def setup():
    """Make the environment, or make it again when it is broken or the pins changed; returns its Python."""
    d = venv_dir()
    if ready(d):
        return venv_python(d)
    lock = _lock(d)
    try:
        if ready(d):                                   # another tool made it while this one waited
            return venv_python(d)
        base = base_python()
        want = ', '.join('%s %s' % p for p in pins())
        log('setting up %s for the Dissidia tools (one time, about 60 MB): %s' % (want, d))
        shutil.rmtree(d, ignore_errors=True)
        if subprocess.run([base, '-m', 'venv', d], stdout=_stderr()).returncode:
            raise SystemExit('[pydeps] "%s -m venv" failed (above). On Debian/Ubuntu: sudo apt install python3-venv'
                             % base)
        py = venv_python(d)
        cmd = [py, '-m', 'pip', 'install', '--disable-pip-version-check', '--no-input', '--only-binary', ':all:',
               '--require-hashes', '-r', REQUIREMENTS]           # only the wheels whose sha256 requirements.txt lists
        if subprocess.run(cmd, stdout=_stderr()).returncode:
            raise SystemExit('[pydeps] pip could not install %s (above; the first run needs the internet). Or '
                             'install them yourself and set DISSIDIA_VENV=0.' % want)
        if not ready(d):
            raise SystemExit('[pydeps] %s was set up but does not have %s' % (d, want))
        log('ready: %s' % py)
        return py
    finally:
        try:
            os.rmdir(lock)
        except OSError:
            pass


def require():
    """Call when the tool runs as a script, before it imports numpy / PIL / scipy (see the module doc)."""
    if os.environ.get('DISSIDIA_VENV') == '0':
        return
    bad = missing()
    if not bad:
        return
    if os.environ.get(GUARD):
        raise SystemExit('[pydeps] %s: %s; delete that folder and run again' % (venv_dir(), describe(bad)))
    if 'bpy' in sys.modules:                           # never replace Blender's own process
        raise ImportError('[pydeps] run the tools as scripts, not inside Blender: %s' % describe(bad))
    if not (sys.argv and os.path.isfile(sys.argv[0])):
        raise SystemExit('[pydeps] %s; run "python3 %s" and use the python it prints'
                         % (describe(bad), os.path.abspath(__file__)))
    py = setup()
    env = {k: v for k, v in os.environ.items() if k not in ('PYTHONHOME', 'PYTHONPATH')}
    env[GUARD] = '1'
    argv = [py] + (['-I'] if sys.flags.isolated else []) + sys.argv
    sys.stdout.flush()
    sys.stderr.flush()
    if os.name == 'nt':                                # no exec on Windows: run it, pass its exit code on
        sys.exit(subprocess.call(argv, env=env))
    os.execve(py, argv, env)


def main(argv):
    if argv[:1] in (['-h'], ['--help']):
        print(__doc__)
        return 0
    if argv[:1] == ['--ok']:                           # used by ready(): does this Python have every pin?
        return 1 if missing() else 0
    print(setup())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
