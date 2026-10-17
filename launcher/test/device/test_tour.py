"""Discover a GraphWalker model and cover every directed lifecycle edge."""
import itertools
import json
from pathlib import Path

import networkx as nx

from device_capture import parse_apps
from console_protocol import replies_to


def discover(console):
    reply = console.exchange("APPS", r"APPS(?!_END)[\s\S]*?APPS_END|APPS_END")
    lines, complete = replies_to(reply.encode(), "APPS", ("APPS_END",))
    assert complete, reply
    apps = parse_apps(lines)
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
    for app in apps:
        name = app["name"]
        graph.add_node(name, name=name)
        graph.add_edge("home", name, command="OPEN " + name)
        graph.add_edge(name, "home", command="HOME")
        for key, _ in app["scenes"]:
            state = (name, key)
            graph.add_node(state, name=name + ": " + key)
            graph.add_edge(name, state, command=f"{app['console']} scene {key}")
            graph.add_edge(state, name, command="OPEN " + name)
        for (source, _), (target, _) in itertools.permutations(app["scenes"], 2):
            graph.add_edge((name, source), (name, target), command=f"{app['console']} scene {target}")
    for source, target in itertools.permutations(apps, 2):
        graph.add_edge(source["name"], target["name"], command="OPEN " + target["name"])
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
    Path(console.dut.logfile).with_name("tour-model.json").write_text(json.dumps(graphwalker(graph), indent=2) + "\n")
    assert nx.is_eulerian(graph)
    for source, target in nx.eulerian_circuit(graph, source="home"):
        console.move(graph[source][target]["command"])
