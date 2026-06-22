# Authoritative upstream source policy

`meraki-builder` consumes `Gadorach/meraki-redboot` and
`Gadorach/postmerkos-ui` as authoritative upstream projects.

By default it fetches and resets meraki-redboot to `origin/main` and
postmerkos-ui to `origin/ms42p-dev` immediately before use. `LOADER_REF=latest` remains accepted only as a compatibility alias
for `main`; it no longer means the newest version tag. An exact loader commit
may be supplied explicitly when a reproducible historical build is required.

The builder must not:

- apply patches to either checkout;
- create repair commits in either checkout;
- silently continue from a stale release tag;
- build from a dirty checkout;
- rewrite upstream source to satisfy a builder-side test.

All loader, recovery, and UI changes belong in their respective repositories.
The builder validates the source contracts it needs and stops with a clear error
when the selected authoritative branches and the builder are out of sync.

The exact selected commit is recorded in the generated artifact provenance.
