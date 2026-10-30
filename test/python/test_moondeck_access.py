"""MoonDeck acts only for this machine: the LAN reads the console and pulls firmware, and a POST, which runs a script, flashes a board or rewrites the state, is this machine's alone.

These pin _may_act, the one gate do_POST asks, so each way in is named: a peer on the network, another site in this machine's browser, a name rebound to 127.0.0.1, and the console itself.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))

from moondeck import PORT, _may_act  # noqa: E402

OWN = {"127.0.0.1", "::1", "192.168.1.20"}


def test_the_console_on_this_machine_acts_by_any_of_its_own_names():
    assert _may_act("127.0.0.1", f"http://localhost:{PORT}", OWN)
    assert _may_act("127.0.0.1", f"http://127.0.0.1:{PORT}", OWN)
    assert _may_act("192.168.1.20", f"http://192.168.1.20:{PORT}", OWN)


def test_a_script_on_this_machine_sends_no_origin_and_acts():
    assert _may_act("127.0.0.1", None, OWN)


def test_a_peer_on_the_network_reads_but_never_acts():
    assert not _may_act("192.168.1.42", None, OWN)
    assert not _may_act("192.168.1.42", f"http://192.168.1.20:{PORT}", OWN)


def test_another_site_open_in_this_machines_browser_is_refused():
    assert not _may_act("127.0.0.1", "https://example.com", OWN)
    assert not _may_act("127.0.0.1", "http://localhost:8080", OWN)   # another local server, such as the MoonLight app


def test_a_name_rebound_to_this_machine_is_refused_by_its_origin():
    assert not _may_act("127.0.0.1", f"http://attacker.example:{PORT}", OWN)


def test_a_malformed_origin_is_refused_rather_than_raising():
    assert not _may_act("127.0.0.1", "http://localhost:abc", OWN)   # a port that is not a number
    assert not _may_act("127.0.0.1", "http://[::1", OWN)            # a host urlsplit cannot parse
