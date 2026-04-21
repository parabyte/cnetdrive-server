# cnetdrive-server

`cnetdrive-server` is a standalone UDP server for the mTCP NetDrive DOS client.

This tree has been updated for compatibility with the newer NetDrive DOS client builds, including:

- protocol v3 request and response framing
- larger I/O sizes used by current clients
- correct `WRITE_VERIFY` reply behavior for `VERIFY ON`

## Build

```sh
./configure
make
```

## Run

Serve exports from the current directory:

```sh
./cnetdrive serve
```

Serve exports from a specific directory:

```sh
./cnetdrive serve -root ./exports
```

Useful options:

- `-root DIR` or `-image_dir DIR` or `-export_dir DIR`: export root directory
- `-port N`: UDP port, default `2002`
- `-max_sessions N`: maximum concurrent sessions
- `-timeout N`: idle session timeout in seconds

## DOS Client Example

Mount an export called `disk.dsk` on drive `D:`:

```dos
ND C 192.168.1.10:2002 disk.dsk D:
```

Disconnect it later:

```dos
ND D D:
```

## Export Notes

- Disk images are served from the export root by filename.
- Host folders can also be exported by directory name.
- Make an image read-only with filesystem permissions.
- Add `<image>.session_scoped` next to an image to keep writes session-local.
- Add `<image>.journal` next to an image to enable checkpoints.

## License

GNU GPL v3 or later. See [LICENSE](LICENSE).
