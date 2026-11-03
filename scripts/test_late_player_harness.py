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
    def test_rule_decision_is_not_overwritten_by_the_sent_cap(self):
        parsed = harness.parse_s1('[session 1] S1 raise player=7 tick=100 k=79 rule=shared-absolute offset=0; '
                                  'lead player=7 id=1 k=53 decided=79 N=101\n'
                                  '[session 1] S1 lead-ack player=7 id=1 applied=53 tick=120 offset=53\n')
        self.assertEqual(parsed['decisions'][0]['k'], 79)
        self.assertEqual(parsed['decisions'][1]['offset'], 53)

    def test_relay_allowance_is_bounded_in_time_and_height(self):
        import arena_map_test as arena
        members = [dict(index=index, id=10 + index, relay_samples=[]) for index in range(4)]
        report = dict(arm='A5P', lead_adjust='on', clients=members, event_clients=[2, 3],
                      t_on=100, phases={'D': [100, 168]})
        band = dict(report, arm='A0', lead_adjust='off', clients=[dict(member, relay_samples=[[100, 122, 22, 11]])
                                                                 for member in members])
        twin = dict(band, arm='A5P')
        member = dict(members[0], relay_samples=[[100, 132, 32, 11], [109, 132, 23, 11], [110, 132, 22, 11]])
        self.assertTrue(all(result['ok'] for result in arena.relay_start_hold(report, member, [band, twin])))
        for sample in ([110, 133, 23, 11], [100, 133, 33, 11]):
            member['relay_samples'][0] = sample
            self.assertFalse(arena.relay_start_hold(report, member, [band, twin])[0]['ok'])
        report['event_clients'] = [2]
        member['relay_samples'] = [[100, 126, 26, 11], [104, 126, 22, 11]]
        self.assertTrue(arena.relay_start_hold(report, member, [band, twin])[0]['ok'])
        member['relay_samples'][0] = [104, 127, 23, 11]
        self.assertFalse(arena.relay_start_hold(report, member, [band, twin])[0]['ok'])
        report['relay_episodes'] = [dict(tick=100, clients=[2]), dict(tick=130, clients=[0])]
        member['relay_samples'] = [[100, 126, 26, 11], [130, 156, 26, 11], [140, 162, 22, 11]]
        self.assertTrue(arena.relay_start_hold(report, member, [band, twin])[0]['ok'])

    def test_majority_relay_uses_last_raise_effective_tick(self):
        import arena_map_test as arena
        member = dict(index=0, id=10, span=[0, 300], relay_samples=[[100, 144, 44, 11], [248, 270, 22, 11]])
        report = dict(arm='A3M', t_on=100, event_clients=[1, 2, 3], phases=dict(D=[100, 150]),
                      clients=[member, dict(index=1, id=11, leads=[dict(e=180, k=20)]),
                               dict(index=2, id=12, leads=[dict(e=190, k=20)]),
                               dict(index=3, id=13, leads=[dict(e=200, k=20)])])
        band = dict(arm='A0', lead_adjust='off', clients=[dict(index=0, id=20, relay_samples=[[100, 122, 22, 21]]),
                                                               dict(index=1, id=21)])
        result = arena.relay_start_hold(report, member, [band])[0]
        self.assertTrue(result['ok'])
        self.assertEqual(result['last_effective'], 200)
        member['relay_samples'][0][2] = 45
        self.assertFalse(arena.relay_start_hold(report, member, [band])[0]['ok'])
        member['relay_samples'][0][2] = 44
        member['relay_samples'][1][2] = 23
        self.assertFalse(arena.relay_start_hold(report, member, [band])[0]['ok'])
        member['relay_samples'][1][0] = 247
        member['relay_samples'].append([249, 271, 22, 11])
        self.assertTrue(arena.relay_start_hold(report, member, [band])[0]['ok'])

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
