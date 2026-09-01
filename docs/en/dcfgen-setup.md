[中文](../zh/dcfgen-setup.md)

# dcfgen setup

The project keeps the source configuration in `config/master.yml` and `config/project.eds`. Generated master/concise DCF files live under `config/generated/`.

The configuration comments currently target the Lely `dcfgen` 2.4.0 workflow used by the project.

## Generate

With a matching `dcf-tools` environment available:

```sh
cd config
<path-to-dcfgen> -r -v -d generated master.yml
```

The earlier project setup used a local `.venv-dcf-tools` virtual environment; that directory is intentionally not part of the repository contract.

## Review before deployment

After generation, review at least:

- master Node-ID and bitrate;
- `0x1F80` NMT startup behavior;
- `0x1F81` slave assignment/boot policy;
- heartbeat producer/consumer settings;
- generated concise DCF for Node 1;
- any PDO/SYNC/EMCY communication parameters changed from the EDS defaults.

Regeneration is required when the baseline communication configuration or source EDS changes. Runtime-only temporary test values should normally be restored by the owning process instead of being persisted into the configuration source.

## Detailed reference

The earlier Chinese setup note is retained at `../zh/reference/dcfgen-setup.md`.
