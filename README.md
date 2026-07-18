## About

Transmission is a fast, easy, and free BitTorrent client. It comes in several flavors:
  * A native macOS GUI application
  * GTK+ and Qt GUI applications for Linux, BSD, etc.
  * A Qt-based Windows-compatible GUI application
  * A headless daemon for servers and routers
  * A web UI for remote controlling any of the above
  
Visit https://transmissionbt.com/ for more information.

## Documentation

[Transmission's documentation](docs/README.md) is currently out-of-date, but the team has recently begun a new project to update it and is looking for volunteers. If you're interested, please feel free to submit pull requests!

### Nashawk Usenet piece backend

Nashawk adds an experimental Usenet-backed piece storage mode for
`transmission-daemon`. When enabled, verified BitTorrent pieces can be uploaded
as one or more deterministic yEnc Usenet articles per piece, later restored on
demand, and optionally evicted from local torrent files to reduce disk usage on
storage-constrained hosts. The current implementation also exposes Usenet
serving state in the Web UI and can sample deterministic piece Message-IDs
after magnet metadata is available, allowing a node to discover torrents that
another Nashawk node has already populated in Usenet. Fully servable
Usenet-backed torrents use seed-like activity and peer behavior without
rewriting normal local completion or tracker completion state.

The BitTorrent picker excludes pieces already available through Usenet. When a
missing piece repeatedly fails its BitTorrent SHA-1 check, Nashawk isolates
subsequent attempts to one peer at a time, rejects previously failing peer IPs,
and applies bounded retry cooldowns instead of downloading corrupt data in an
unbounded loop.

Start with [Nashawk Usenet piece backend](docs/Usenet-Piece-Backend-README.md)
for setup, safety notes, daemon flags, and validation steps.

## Command line interface notes

Transmission is fully supported in transmission-remote, the preferred cli client.

Three standalone tools to examine, create, and edit .torrent files exist: transmission-show, transmission-create, and transmission-edit, respectively.

Prior to development of transmission-remote, the standalone client transmission-cli was created. Limited to a single torrent at a time, transmission-cli is deprecated and exists primarily to support older hardware dependent upon it. In almost all instances, transmission-remote should be used instead.

Different distributions may choose to package any or all of these tools in one or more separate packages.

## Building

Transmission has an Xcode project file (Transmission.xcodeproj) for building in Xcode.

For a more detailed description, and dependencies, visit [How to Build Transmission](docs/Building-Transmission.md) in docs

### Building a Transmission release from the command line

```bash
$ tar xf transmission-4.1.0.tar.xz
$ cd transmission-4.1.0
# Use -DCMAKE_BUILD_TYPE=RelWithDebInfo to build optimized binary with debug information. (preferred)
# Use -DCMAKE_BUILD_TYPE=Release to build full optimized binary.
$ cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cd build
$ cmake --build .
$ sudo cmake --install .
```

### Building Transmission from the nightly builds

Download a tarball from https://build.transmissionbt.com/job/trunk-linux/ and follow the steps from the previous section.

If you're new to building programs from source code, this is typically easier than building from Git.

### Building Transmission from Git (first time)

```bash
$ git clone --recurse-submodules https://github.com/transmission/transmission Transmission
$ cd Transmission
# Use -DCMAKE_BUILD_TYPE=RelWithDebInfo to build optimized binary with debug information. (preferred)
# Use -DCMAKE_BUILD_TYPE=Release to build full optimized binary.
$ cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cd build
$ cmake --build .
$ sudo cmake --install .
```

### Building Transmission from Git (updating)

```bash
$ cd Transmission/build
$ cmake --build . -t clean
$ git submodule foreach --recursive git clean -xfd
$ git pull --rebase --prune
$ git submodule update --init --recursive
$ cmake --build .
$ sudo cmake --install .
```

## Contributing

### Code Style

You would want to setup your editor to make use of the .clang-format file located in the root of this repository and the eslint/prettier rules in web/package.json.

If for some reason you are unwilling or unable to do so, there is a shell script which you can use: `./code_style.sh`

### Translations

See [language translations](docs/Translating.md).

## Sponsors

<table>
 <tbody>
  <tr>
   <td align="center"><img alt="[MacStadium]" src="https://uploads-ssl.webflow.com/5ac3c046c82724970fc60918/5c019d917bba312af7553b49_MacStadium-developerlogo.png" height="30"/></td>
   <td>macOS CI builds are running on a M1 Mac Mini provided by <a href="https://www.macstadium.com/company/opensource">MacStadium</a></td>
  </tr>
  <tr>
   <td align="center"><img alt="[SignPath]" src="https://avatars.githubusercontent.com/u/34448643" height="30"/></td>
   <td>Free code signing on Windows provided by <a href="https://signpath.io/?utm_source=foundation&utm_medium=github&utm_campaign=transmission">SignPath.io</a>, certificate by <a href="https://signpath.org/?utm_source=foundation&utm_medium=github&utm_campaign=transmission">SignPath Foundation</a></td>
  </tr>
 </tbody>
</table>
