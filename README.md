# weemitt

A native PC port of the N64 game Glover (USA), built from the C of the matching decompilation [gloveworld](https://github.com/Eddie-Lack/gloveworld). No game code is included, you must use your own copy of the Glover ROM

[docs/playing.md](docs/playing.md) is the guide for players, and [docs/README.md](docs/README.md) lists the other docs.

## Building

```sh
make -j4              # Linux, 32-bit: build/linux/weemitt
make windows -j4      # Windows, 32-bit: build/windows/weemitt.exe
make linux64 -j4      # Linux, 64-bit: build/linux64/weemitt
make windows64 -j4    # Windows, 64-bit: build/windows64/weemitt.exe
```

[docs/building.md](docs/building.md) lists the packages each build needs and how to build SDL2 for Windows.

```sh
./build/linux/weemitt --rom /path/to/glover.z64
```

## License

The port's own code is under the MIT license ([LICENSE](LICENSE)). The license covers this code only. It gives
no right to the game, its ROM or its data. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists the code from
other projects and its licenses.
