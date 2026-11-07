"""MoonAtlas's 3D view draws the cloud it is given, counts what it shows, and asks MoonAtlas about the name a reader types: checked in a real browser where Playwright is installed (`uv run moondeck/docs/install_playwright.py`), skipped where it is not.

The page is served from moondeck_ui/ as MoonDeck serves it, and its two routes answer with a graph of three nodes written for the test, so the check pins the page rather than the repository.
"""

import http.server
import json
import threading
import urllib.parse
from pathlib import Path

import pytest

UI = Path(__file__).resolve().parents[2] / "moondeck" / "moondeck_ui"
GRAPH = {"verb": "cloud", "query": None, "graph": {"fresh": True, "note": "MoonAtlas: up to date"},
         "nodes": [{"id": "module:Layer", "kind": "module", "name": "Layer", "path": None, "line": None, "area": "src/light"},
                   {"id": "file:src/light/Layer.h", "kind": "file", "name": "Layer.h", "path": "src/light/Layer.h", "line": None, "area": "src/light"},
                   {"id": "claim:c.json#c-001", "kind": "claim", "name": "A claim.", "path": "c.json", "line": 3, "area": "src/light", "stale": True}],
         "links": [{"source": "file:src/light/Layer.h", "target": "module:Layer", "rels": ["defined_in", "is"], "weight": 3},
                   {"source": "claim:c.json#c-001", "target": "file:src/light/Layer.h", "rels": ["cites"], "weight": 1}]}


@pytest.fixture
def served():
    """The page and its vendored library from moondeck_ui/, the atlas routes answering from GRAPH, and every `what` asked recorded."""
    asked = []

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=str(UI), **k)

        def log_message(self, *a):
            pass

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            if not url.path.startswith("/api/atlas/"):
                return super().do_GET()
            if url.path == "/api/atlas/graph":
                body = GRAPH
            else:
                name = urllib.parse.parse_qs(url.query)["name"][0]
                asked.append(name)
                body = {"verb": "what", "query": name, "text": [f"module:{name}: One rendering layer."], "note": ""}
            data = json.dumps(body).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{srv.server_address[1]}/atlas.html", asked
    srv.shutdown()


@pytest.fixture
def page():
    """A page in the browser install_playwright.py installs, else the one installed; skipped without either."""
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        browser = None
        for kind in (p.chromium, p.webkit):
            try:
                browser = kind.launch()
                break
            except Exception:
                continue
        if browser is None:
            pytest.skip("no Playwright browser installed")
        page = browser.new_page()
        page.errors = []
        page.on("pageerror", lambda e: page.errors.append(str(e)))
        yield page
        browser.close()


def test_the_view_draws_the_cloud_counts_it_and_asks_about_the_name_typed(served, page):
    url, asked = served
    page.goto(url)
    page.wait_for_function("document.getElementById('status').textContent.includes('nodes')", timeout=30000)
    assert page.inner_text("#status") == "3 nodes, 2 links, 1 stale claim"
    assert page.locator("#graph canvas").count() == 1
    page.fill("#name", "Layer")
    page.press("#name", "Enter")
    page.wait_for_function("document.getElementById('answer').textContent.startsWith('module:')", timeout=30000)
    assert asked == ["Layer"]   # the name as typed, not the node it found
    assert page.inner_text("#answer") == "module:Layer: One rendering layer."
    page.uncheck("input[data-kind=claim]")
    assert page.inner_text("#status") == "2 nodes, 1 links"   # the stale claim hidden with its kind
    assert page.errors == []


def test_a_graph_route_that_answers_with_a_page_rather_than_json_shows_its_error_instead_of_hanging(served, page):
    url, _ = served
    page.route("**/api/atlas/graph", lambda route: route.fulfill(status=404, content_type="text/html", body="<h1>Not Found</h1>"))
    page.goto(url)
    page.wait_for_function("!/^(loading|asking)/.test(document.getElementById('status').textContent)", timeout=30000)
    assert page.inner_text("#status").startswith("MoonDeck did not answer /api/atlas/graph")
    assert page.errors == []
