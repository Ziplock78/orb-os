"""Send Orb USB commands without toggling the device's reset lines."""
import argparse
import json
import time
import serial


class Orb:
    def __init__(self, port):
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.3)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()

    def command(self, command, timeout=12):
        self.serial.reset_input_buffer()
        self.serial.write(('?orb ' + command + '\n').encode())
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.serial.readline().decode(errors='replace').strip()
            if '!orb ' in line:
                reply = json.loads(line.split('!orb ', 1)[1])
                if reply.get('ok') is False:
                    raise RuntimeError(reply)
                return reply
        raise TimeoutError(command)

    def close(self):
        self.serial.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM5')
    parser.add_argument('commands', nargs='+')
    args = parser.parse_args()
    orb = Orb(args.port)
    try:
        for command in args.commands:
            print(command, json.dumps(orb.command(command)))
            time.sleep(0.3)
    finally:
        orb.close()
