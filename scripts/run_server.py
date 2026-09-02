"""Runs BallanceMMOServer with a live stdin pipe and forwards console commands.

usage: run_server.py <exe> <workdir> <stdout log> <command file>
Every line written to the command file is sent to the server console (the
file is deleted after reading); a line "stop" also ends this launcher.
"""
import os
import subprocess
import sys
import time

exe, workdir, log_path, cmd_path = sys.argv[1:5]
log = open(log_path, 'ab', buffering=0)
proc = subprocess.Popen([exe], cwd=workdir, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
print('server pid', proc.pid, flush=True)
stopping = False
while proc.poll() is None:
    if os.path.exists(cmd_path):
        try:
            with open(cmd_path, 'r', encoding='utf-8') as f:
                lines = f.read().splitlines()
            os.remove(cmd_path)
        except OSError:
            lines = []
        for line in lines:
            line = line.strip()
            if not line:
                continue
            try:
                proc.stdin.write((line + '\n').encode('utf-8'))
                proc.stdin.flush()
            except OSError:
                break
            if line == 'stop':
                stopping = True
    if stopping:
        for _ in range(50):
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        if proc.poll() is None:
            proc.kill()
        break
    time.sleep(0.2)
print('server exited', proc.poll(), flush=True)
