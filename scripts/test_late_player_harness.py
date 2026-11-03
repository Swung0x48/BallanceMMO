"""Offline LB0 checks; no game, server or network connection is needed."""
import json
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import late_player_harness as harness


class Recipes(unittest.TestCase):
    def test_small_sessions_never_target_a_missing_client(self):
        for name, count in [('A5U2', 2), ('A5U3', 3), ('A5S2', 2), ('A11', 1), ('A11U', 1)]:
            r = harness.recipe(name)
            self.assertEqual(r['players'], count)
            self.assertTrue(all(int(v.split(':')[0]) < count for v in r['events']))

    def test_distinct_faults_and_heterogeneous_links(self):
        self.assertEqual(harness.recipe('A0H')['links'], ['0:lat=50', '1:lat=150', '2:lat=150', '3:lat=250'])
        self.assertEqual([int(v.split(':')[0]) for v in harness.recipe('A5P')['events']], [2, 3])
        self.assertEqual([int(v.split(':')[0]) for v in harness.recipe('A3M')['events']], [1, 2, 3])
        self.assertEqual(len(harness.recipe('A10X')['events']), 4)
        self.assertIn('2100:spike=300', harness.recipe('A5UB')['events'][1])
        self.assertEqual(harness.recipe('A9H20')['clock_skew'], '300:20000:7.5')
        for name in harness.ARMS:
            r = harness.recipe(name)
            self.assertTrue(r['events'] or r['client_args'] or r['join_late'])

    def test_dry_run_and_client_alias_are_safe(self):
        p = subprocess.run([sys.executable, '-I', str(pathlib.Path(__file__).with_name('arena_map_test.py')),
                            '--scenario', 'A5P', '--clients', '4', '--twins', '--dry-run'],
                           capture_output=True, text=True, timeout=15)
        self.assertEqual(p.returncode, 0, p.stderr)
        r = json.loads(p.stdout)
        self.assertEqual(r['players'], 4)
        self.assertEqual(r['event'], ['2:1500:step=300:dir=up', '3:1500:step=300:dir=up'])


class ClockDiagnostics(unittest.TestCase):
    def test_injected_span_includes_recovery_and_filters_sessions(self):
        text = ('[session 7] S1 clock-skew begin tick=300 ms=8000 percent=7.5\n'
                '[session 8] S1 clock-skew end tick=999\n'
                '[session 7] S1 clock-skew recover tick=829\n'
                '[session 7] S1 clock-skew end tick=1357\n')
        parsed = harness.parse_s1(text, 7)
        self.assertIsNone(parsed['quorum'])
        clock = harness.host_clock(parsed, {1: 15, 2: 15})
        self.assertEqual(clock['label'], 'host clock')
        self.assertEqual(clock['spans'][0]['last'], 1357)
        self.assertEqual(clock['spans'][0]['recover'], 829)

    def test_shared_network_delay_is_not_a_host_clock_fault(self):
        rows = '\n'.join('[session 1] S1 lateness player=%d tick=660 q50=-5.2' % i for i in range(4))
        parsed = harness.parse_s1(rows)
        cadence = {i: 15 for i in range(4)}
        self.assertIsNone(harness.host_clock(parsed, cadence, {660: 15})['label'])
        clock = harness.host_clock(parsed, cadence, {660: 14})
        self.assertEqual(clock['label'], 'host clock')
        self.assertEqual(clock['spans'][0]['source'], 'observed')
        self.assertIsNone(harness.host_clock(parsed, {0: 20, 1: 15, 2: 15, 3: 15}, {660: 14})['label'])

    def test_future_quorum_sector_and_decision_rows(self):
        parsed = harness.parse_s1('[session 1] S1 quorum tick=660 M=4 M2=10 counted=3 gap_p99=1.5\n'
                                  '[session 1] S1 lateness player=7 tick=660 q50=3.4 union=4/4 sector-late=2\n'
                                  '[session 1] S1 would-raise2 player=7 tick=700 rule=relative k=20\n')
        self.assertEqual(parsed['quorum'][0]['M2'], 10)
        self.assertEqual(parsed['sector'][0]['union'], '4/4')
        self.assertEqual(parsed['decisions'][0]['action'], 'would-raise2')
        self.assertIsNone(parsed['arrival'])


class Twins(unittest.TestCase):
    def test_off_then_selected_flags_with_control(self):
        with tempfile.TemporaryDirectory() as directory:
            args = types.SimpleNamespace(external_server='127.0.0.1:1', server_control='owned.py',
                                         label='pair', out=directory, twin_flags='patience', scenario='A0', arm='')
            commands = []
            def run(cmd, **kwargs):
                commands.append(cmd)
                index = len(cmd) - 1 - cmd[::-1].index('--label')
                path = pathlib.Path(directory) / cmd[index + 1]
                path.mkdir()
                (path / 'summary.json').write_text(json.dumps([{'sector': 1, 'clients': [], 'event_report': {},
                                                               'criteria': [{'status': 'pass', 'criterion': 'control band'}]}]))
                return types.SimpleNamespace(returncode=0)
            with mock.patch.object(harness.subprocess, 'run', side_effect=run):
                self.assertEqual(harness.twins(args, ['--twins', '--scenario', 'A0']), 0)
            self.assertIn('--control', commands[1])
            self.assertEqual(commands[0][commands[0].index('--patience') + 1], 'off')
            self.assertEqual(commands[1][commands[1].index('--patience') + 1], 'on')
            self.assertEqual(commands[1][commands[1].index('--lead-adjust') + 1], 'off')
            self.assertNotIn('--twins', commands[0])
            saved = json.loads((pathlib.Path(directory) / 'pair_twins.json').read_text())
            self.assertEqual(saved['sectors'][0]['criteria'][0]['criterion'], 'control band')


if __name__ == '__main__':
    unittest.main()
