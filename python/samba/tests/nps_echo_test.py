# Unix SMB/CIFS implementation.
#
# # Copyright (C) Noel Power <npower@samba.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

from samba.samba3 import libsmb_samba_internal as libsmb
from samba.samba3 import param as s3param
import samba.tests
import os
from samba.credentials import Credentials

class TestNPSEcho(samba.tests.TestCase):

    def setUp(self):
        super().setUp()
        self.lp = s3param.get_context()
        self.server_ip = os.environ["SERVER_IP"]
        self.user = os.environ["USER"]
        self.passwd = os.environ["PASSWORD"]
        self.creds = Credentials()
        self.creds.guess(self.lp)
        self.creds.set_username(self.user)
        self.creds.set_password(self.passwd)

    def _test_simple_echo(self, pipe_name):
        c = libsmb.Conn(
            self.server_ip,
            "ipc$",
            self.lp,
            self.creds)

        fnum = c.create(pipe_name, CreateDisposition=libsmb.FILE_OPEN);
        echoes = [b'one', b'two', b'three', b'four']
        for echo in echoes:
            c.write(fnum, echo, len(echo), 0)
            buf = c.read(fnum, 0, 4096)
            self.assertEqual(buf, echo)

    def test_simple_echo_nps_echo_msg8(self):
        return self._test_simple_echo("nps_echo_msg8")

    def test_simple_echo_nps_ECHO_msg8(self):
        return self._test_simple_echo("nps_ECHO_msg8")

    def test_simple_echo_nps_echo_msg16(self):
        return self._test_simple_echo("nps_echo_msg16")

    def test_simple_echo_nps_ECHO_msg16(self):
        return self._test_simple_echo("nps_ECHO_msg16")
