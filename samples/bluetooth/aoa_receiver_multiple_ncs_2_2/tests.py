#!/usr/bin/env python
import unittest
import time
from datetime import datetime
import sys
import sys, serial, argparse
from ubx_serial.SPA import SPA
import threading
import time

class TestFilter(unittest.TestCase):

    def setUp(self):
        port = serial.Serial(port=sys.port, baudrate=115200, rtscts=True, timeout=2)
        port.reset_input_buffer()
        port.reset_output_buffer()
        self.spa = SPA(port)

        # Make sure it's alive
        self.sendCommand("AT", "Failed communicating with tag")


    def tearDown(self):
        pass

    def test_tag_sync(self):
        pass


    def sendCommand(self, cmd, msg='', timeout=1):
        res = self.spa.command(cmd, timeout=timeout)
        self.assertIsNot(res, -1, msg=msg)
        return res

    def current_milli_time(self):
        return round(time.time() * 1000)

    def parseEvent(self, urc_str):
        splitat = urc_str.find(':')

        urc, r = urc_str[:splitat], urc_str[splitat:]
        if urc.upper() != "+UUDF":
            return None

        urc_params = r.split(',')
        instanceId = urc_params[0][1:]

        urc_dict = {
            "instanceId": instanceId,
            "rssi": int(urc_params[1]),
            "angleH": int(urc_params[2]),
            "angleV": int(urc_params[3]),
            "rssi2": int(urc_params[4]),
            "channel": int(urc_params[5]),
            "anchor_id": urc_params[6].replace("\"", ""),
            "user_defined_str": urc_params[7].replace("\"", ""),
            "timestamp_ms": int(urc_params[8]),
        }
        return urc_dict

if __name__=='__main__':
    parser = argparse.ArgumentParser(description="AoA Tester")
    parser.add_argument('--receiver_port', dest='receiver_port', required=True)
    parser.add_argument('--tag_ports', dest='tag_ports', required=True)
    parser.add_argument('--tag_ports','--tag_ports', nargs='+', help='list of tag ports', required=True)

    args, unknown = parser.parse_known_args()
    print(args.tag_ports)
    sys.port = args.port
    sys.antenna = args.antenna

    sys.argv = sys.argv[:1] + unknown
    unittest.main()
