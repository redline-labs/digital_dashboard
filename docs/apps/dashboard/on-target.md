---
title: Config on the target
parent: dashboard
grand_parent: Apps
redirect_from: /config-on-target.html
---

# Changing the dashboard's config on a running board

The config the image ships lives in the read-only slot,
`/usr/share/redline-dashboard/mercedes_190e_dash.yaml`. To change what the
cluster shows without reflashing, put a config on the data partition:

```
/data/dashboard/config.yaml
```

The dashboard's unit passes it as `--config-override`. When the file is present
and valid it replaces the shipped config; when it is absent nothing changes.

## Workflow

```sh
# on the board, or scp the file in
dashboard --check --config /data/dashboard/config.yaml     # loads it and builds its windows headless; exit 0 = OK
systemctl restart redline-dashboard
systemctl status redline-dashboard                        # the Status: line says which config is running
```

`--check` runs the full validation, the same the dashboard applies at startup,
and prints the reasons when it refuses.

## What happens with a bad override

The override is **rejected** when the loader refuses it (the rules in
[displays.md](windows.md): unknown keys, duplicate window names, two windows on
one display...) or when any of its widgets fails to construct. The dashboard
then:

- logs the reason (`journalctl -u redline-dashboard`);
- runs the shipped config, and still reports `READY` to systemd, because the
  slot is fine and must not roll back;
- shows a red banner in the top-left corner of the cluster saying the override
  was rejected and why;
- puts the same sentence in the unit's `Status:` line.

The rejected file is left exactly where it was, so nothing is lost: fix it,
`--check` it, restart.

## Why the slot is never rolled back for this

The data partition is shared by both RAUC slots. If a bad override could fail
the boot, the rolled-back slot would meet the same file and fail too, and the
board would bounce between two "bad" slots with nothing actually wrong with
either. So override failures are the application's to handle, and slot health
means "the shipped software and config work", which is exactly what the boot
counter should measure.

## The attempt marker

Validation cannot see an override that loads and then crashes the process. That
would restart-loop, never reach `READY`, and roll both slots back in turn. So
starting with an override writes `/data/dashboard/config.yaml.attempt`, and the
dashboard removes it once its first frame is on screen. If the marker is already
there at startup, the last attempt never got that far: the override is skipped
for this boot, the shipped config runs, and the banner says so. Remove the
marker to try again after fixing the file.

## On a desktop

`--config-override` and `--check` work anywhere; nothing here needs the target.
`--check` is the quickest way to validate a config in CI.
