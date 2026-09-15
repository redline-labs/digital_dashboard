---
title: Home
nav_order: 0
---

# Redline Digital Dash

An open, hackable instrument cluster for vehicles. The dashboard draws every
gauge from a YAML layout, the layout is edited in a drag-and-drop editor, and
everything the gauges show arrives over one bus that any hardware node can
publish onto. The same binaries run on the target hardware and on a desktop.

This site is organised by who is reading it.

| Section | For | Start with |
|---|---|---|
| [Getting started](getting-started/) | anyone building from source | the build recipe and a demo layout |
| [Apps](apps/) | people running the GUI programs | [dashboard](apps/dashboard/), [editor](apps/), [scope](apps/scope.html), [switchboard](apps/switchboard.html) |
| [Nodes](nodes/) | people putting hardware on the bus | the table of every node and what it needs |
| [Tools](tools/) | people building map data on a workstation | [map_build](tools/map_build.html) |
| [Libraries](libs/) | developers coding against the tree | the grouped index and each library's page |
| [Reference](reference/) | lookups | [runtime environment](reference/environment.html), third-party licences |
| [Design notes](design/) | the reasoning behind the shape of things | dated working notes, not manuals |
| [Developing](developing/) | people changing the tree | build and test rules, the agent control interface, the how-tos |

The project's motivation and the hardware it is built for are on the
[About](about.html) page. The source is at
[github.com/redline-labs/digital_dashboard](https://github.com/redline-labs/digital_dashboard).
