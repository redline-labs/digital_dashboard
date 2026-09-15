---
title: Writing docs
parent: Developing
nav_order: 5
---

# Writing docs

The `docs/` directory is a Jekyll site built by GitHub Pages on every push to
`main` and published at
[dashboard-docs.redline-labs.com](https://dashboard-docs.redline-labs.com/).
Adding a file there publishes it. This page says where a page goes, what it
needs at the top, and how to write it.

## Which section

Sections are by audience, not by subsystem, so one device usually has pages in
three of them.

| Section | Reader | What goes there |
|---|---|---|
| `apps/` | someone running a GUI program | running it, its options, its configuration, what it shows, troubleshooting |
| `nodes/` | someone putting hardware on the bus | what it bridges, running it with and without the hardware, topics and services, what fails silently |
| `tools/` | someone building data on a workstation | the verbs, the pipeline, checking the output |
| `libs/` | a developer coding against the tree | what it is and is not, public headers, using it, behaviour worth knowing, tests |
| `reference/` | anyone looking something up | tables: environment, licences, bus conventions |
| `design/` | a future developer asking why | what was tried, measured, and decided, with dates |
| `developing/` | someone changing the tree | build and test rules, the agent interface, the how-tos |

A page is named after the thing as it appears in the tree: `nodes/xpr_bridge.md`,
not `nodes/mototrbo.md`, so a grep for the node name finds it. Each section's
`index.md` lists everything in the tree, and a new node or library gets a row
there even before it has a page.

## Front matter

Every page starts with a front matter block; a page without one gets a title
guessed from its heading and lands wherever the sort puts it.

```yaml
---
title: xpr_bridge          # what the sidebar shows; the node/lib name for those sections
parent: Nodes              # the section index's title
nav_order: 2               # optional; siblings sort by title without it
redirect_from: /xpr.html   # only when a page moved; keeps the old URL working
---
```

A page two levels down adds `grand_parent:`. Section indexes carry `nav_order`
and no parent. The theme version is pinned in `_config.yml`; bump it
deliberately.

To check a change before pushing:

```bash
cd docs && bundle install && bundle exec jekyll build --strict_front_matter
```

and read `_site/` for the page and the sidebar.

## The shape of a page

A node or app page: an `## Overview` of two or three sentences (what it is,
what it needs), `## Running it` with the command and an options table,
`## Configuration` if there is a file, `## Topics` and `## Services` as tables
with schema names, and `## Troubleshooting` for the two or three ways it fails
silently. Sixty to two hundred lines.

A library page: an `## Overview` paragraph on what it is, what it deliberately
is not, and why the split exists; `## Public headers` as a table; `## Using it`
with the target name and the entry point; `## Behaviour worth knowing`;
`## Tests` naming the targets and what they prove. Forty to a hundred and fifty
lines; a library with one header and no traps gets forty.

A design note has an `## Overview` saying what the note is, then conventional
headings, and keeps every number and date. It is allowed to be long.

## Voice

Write for a technical reader who did not watch the work.

Use plain section headings a reader can guess before scrolling: "Overview",
"Running it", "Configuration", "Topics", "Troubleshooting", "Tests". Not
"Key Capabilities", "Summary", "Conclusion" or "Best Practices".

Prose over bullets. A bullet list is for parallel items of one clause each; no
nested bullets, and no bullet that is a bold label followed by a sentence.
Bold is for the lead-in of a paragraph a reader is scanning for.

Commands, keys and numbers go in code blocks or tables, not in sentences.

A tip or a gotcha that is one or two standalone sentences and does not belong
to the surrounding paragraph goes in a callout: write the paragraph and put
`{: .note }`, `{: .tip }`, `{: .warning }` or `{: .important }` on the line
before it. Anything longer gets a heading.

Leave out the filler: robust, seamless, powerful, comprehensive, leverage,
streamline, ensure, utilize, "it's important to note", "simply", "just", and
any sentence that restates the one before it. Say what was not verified, with
a date, instead of hedging everywhere.

When restructuring an existing page, keep the sentences that are good. Rewrite
where the audience changed, not for its own sake.
