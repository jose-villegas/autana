"""Discover a GraphWalker model and cover every directed lifecycle edge."""
import itertools
import json
from pathlib import Path

import networkx as nx
import pytest

from device_capture import parse_apps


def discover(console):
    reply = console.exchange("APPS", r"APPS(?!_END)[\s\S]*?APPS_END|APPS_END")
    apps = parse_apps(reply.splitlines())
    assert apps, "APPS listed no apps"
    for app in apps:
        console.move("OPEN " + app["name"])
        app["scenes"] = console.scenes(app["console"]) if app.get("console") else []
        console.move("HOME")
    assert any(len(app["scenes"]) > 1 for app in apps), "no scene pairs found"
    return apps


def model(apps):
    graph = nx.DiGraph()
    graph.add_node("home", name="Home")
    landings = ["home"]
    for app in apps:
        name = app["name"]
        scenes = app["scenes"]
        landing = (name, next(key for key, current in scenes if current)) if scenes else name
        landings.append(landing)
        graph.add_node(landing, name=name if not scenes else name + ": " + landing[1])
        for key, _ in scenes:
            graph.add_node((name, key), name=name + ": " + key)
        for (source, _), (target, _) in itertools.permutations(scenes, 2):
            graph.add_edge((name, source), (name, target), command=f"{app['console']} scene {target}")
    for source, target in itertools.permutations(landings, 2):
        name = target[0] if isinstance(target, tuple) else target
        graph.add_edge(source, target, command="HOME" if target == "home" else "OPEN " + name)
    assert nx.is_eulerian(graph)
    return graph


def graphwalker(graph):
    ids = {node: f"v{index}" for index, node in enumerate(graph)}
    return {"name": "Lifecycle", "models": [{
        "name": "Lifecycle", "generator": "random(edge_coverage(100))", "startElementId": ids["home"],
        "vertices": [{"id": ids[node], "name": data["name"]} for node, data in graph.nodes(data=True)],
        "edges": [{"id": f"e{index}", "name": data["command"], "sourceVertexId": ids[source],
                   "targetVertexId": ids[target]} for index, (source, target, data) in enumerate(graph.edges(data=True))],
    }]}


def test_lifecycle_tour(console):
    graph = model(discover(console))
    path = Path(console.dut.logfile).with_name("tour-model.json")
    path.write_text(json.dumps(graphwalker(graph), indent=2) + "\n")
    saved = json.loads(path.read_text())["models"][0]
    graph = nx.DiGraph()
    graph.add_edges_from((edge["sourceVertexId"], edge["targetVertexId"], {"command": edge["name"]})
                         for edge in saved["edges"])
    assert nx.is_eulerian(graph)
    for source, target in nx.eulerian_circuit(graph, source=saved["startElementId"]):
        console.move(graph[source][target]["command"])


@pytest.mark.parametrize("embedded_services", [""], indirect=True)
def test_model_has_only_state_changes():
    apps = [
        {"name": "gallery", "console": "VIEW", "scenes": [("a", False), ("b", True), ("c", False)]},
        {"name": "clock", "console": "", "scenes": []},
        {"name": "demo", "console": "DEMO", "scenes": [("x", True), ("y", False)]},
    ]
    graph = model(apps)
    landings = [("gallery", "b"), "clock", ("demo", "x")]
    assert set(graph) == {"home", "clock", ("gallery", "a"), ("gallery", "b"),
                          ("gallery", "c"), ("demo", "x"), ("demo", "y")}
    for source, target in itertools.permutations(["home", *landings], 2):
        assert graph.has_edge(source, target)
    for app in apps:
        for (source, _), (target, _) in itertools.permutations(app["scenes"], 2):
            assert graph.has_edge((app["name"], source), (app["name"], target))
    assert not list(nx.selfloop_edges(graph))
    for source, target, data in graph.edges(data=True):
        if data["command"].startswith("OPEN "):
            assert (source[0] if isinstance(source, tuple) else source) != data["command"].split()[1]
    assert nx.is_eulerian(graph)
