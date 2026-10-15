import os
import re
import unittest

import remote_config as rc

CONFIG_C = os.path.join(os.path.dirname(__file__), '..', 'Tracker', 'src', 'config.c')


class SettingsTable(unittest.TestCase):
    def test_names_match_the_tracker(self):
        src = open(CONFIG_C, encoding='utf-8').read()
        block = re.search(r'config_settings_names\[\] = \{(.*?)\};', src, re.S)[1]
        self.assertEqual(re.findall(r'"([a-z0-9_]+)"', block), rc.SETTINGS)

    def test_ids(self):
        self.assertEqual(rc.setting_id('user_extra_actions'), 0)
        self.assertEqual(rc.setting_id('sensor_update_time_ms'), 29)
        self.assertEqual(rc.setting_id('sensor_send_unchanged'), len(rc.SETTINGS) - 1)
        self.assertEqual(rc.setting_id('7'), 7)


class ReceiverLines(unittest.TestCase):
    def test_result(self):
        m = rc.RESULT.match('cfg: tracker 3 setting 29 = 10')
        self.assertEqual((m[1], m[2], m[3]), ('3', '29', '10'))
        m = rc.RESULT.match('cfg: idle, last: tracker 3 setting 21 = -4')
        self.assertEqual(m[3], '-4')
        m = rc.RESULT.match('cfg: tracker 3 setting 99: no such setting')
        self.assertEqual(m[4], 'no such setting')

    def test_failed(self):
        m = rc.FAILED.match('cfg: tracker 3 did not answer (off, not paired)')
        self.assertTrue(m[2].startswith('did not answer'))
        m = rc.FAILED.match('cfg: idle, last: tracker 3: stopped, a firmware update started')
        self.assertTrue(m[2].startswith('stopped'))


if __name__ == '__main__':
    unittest.main()
