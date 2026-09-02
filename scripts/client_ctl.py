"""Sends commands to the retail client through the file command channel.

usage: client_ctl.py <command file> [--wait N] <command...>
Writes one line per command, waits for "<file>.out" (the mod writes it on the
game thread after dispatching), prints the responses and removes the file.
"""
import os
import sys
import time

args = sys.argv[1:]
cmd_path = args.pop(0)
wait = 10.0
if args and args[0] == '--wait':
    args.pop(0)
    wait = float(args.pop(0))
commands = args
out_path = cmd_path + '.out'
if os.path.exists(out_path):
    os.remove(out_path)
with open(cmd_path, 'w', encoding='utf-8') as f:
    f.write('\n'.join(commands) + '\n')
deadline = time.time() + wait
while time.time() < deadline:
    if os.path.exists(out_path):
        time.sleep(0.1)
        try:
            with open(out_path, 'r', encoding='utf-8', errors='replace') as f:
                print(f.read().rstrip())
            os.remove(out_path)
        except OSError as e:
            print('read failed:', e)
        sys.exit(0)
    time.sleep(0.1)
print('TIMEOUT: no response within %.1fs (command file %s)' % (wait, 'still present' if os.path.exists(cmd_path) else 'consumed'))
sys.exit(1)
