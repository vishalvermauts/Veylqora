# Operations: backup, restore, upgrade, rollback

These procedures apply to a single local daemon and its database file. The
database is the only state Veylqora keeps; the index can always be rebuilt from
source, but memories and their history cannot.

## Backup

The daemon holds the database lock (`<db>.lock`) while it runs. A backup is
taken with the daemon stopped, so the snapshot is the database as it was when the
daemon last committed.

```bash
veylqora serve --db /data/veylqora.db            # stop with Ctrl-C or SIGTERM
veylqora backup /backups/veylqora-$(date +%F).db --db /data/veylqora.db
```

The backup is written with SQLite's online backup API and then checked with
`PRAGMA integrity_check`. A failed check removes the file and reports an error.
An existing file is never overwritten.

In a container, stop the service first (`docker compose stop veylqora`), then
run the backup with a temporary container that mounts the same volume.

## Restore

```bash
veylqora restore /backups/veylqora-2026-10-21.db --db /data/veylqora.db
```

The backup is checked before anything changes, so a damaged file leaves the
target untouched. The target is then replaced and migrated to this build's
schema if the backup came from an older release. Restore into a new path to
keep the current database as a fallback.

## Upgrade

Migrations are forward-only (ADR-0008): a newer build migrates a database in
place, and an older build refuses a database from a newer one. So an upgrade
always starts with a backup.

1. Stop the daemon.
2. Back up the database to a dated file.
3. Install the new binary (or rebuild the image).
4. Start the daemon. It migrates the database and records the new schema version.
5. Check with `veylqora doctor --db /data/veylqora.db`. It reports integrity,
   foreign-key violations and the schema version.

## Rollback

Because migrations are forward-only, rolling back means restoring the backup
taken in step 2 of the upgrade, with the previous binary:

1. Stop the daemon.
2. Install the previous binary (or the previous image tag).
3. Restore the pre-upgrade backup into the database path.
4. Start the daemon and check with `veylqora doctor`.

Memories written after the backup are lost by a rollback. Record the backup
time before rolling back.

## Container notes

- The image runs the daemon on loopback inside the container. Nothing is
  published to the host, because the daemon refuses non-loopback binds until
  authentication exists (ADR-0012). Use `docker compose exec` for the CLI and the
  MCP server.
- Data lives in the `veylqora-data` volume, mounted at `/data`. Removing the
  container does not remove the volume.
- The healthcheck calls `/api/v1/health` every 30 seconds.

## Verified

The procedures above are exercised by `scripts/e2e.sh` (the `e2e` CI job):
install, start, index, CLI search, MCP connect, restart, backup, restore, and
search again. That run uses the binary directly. The container image is not yet
built or run by CI (see the M3.4 status in the README).
