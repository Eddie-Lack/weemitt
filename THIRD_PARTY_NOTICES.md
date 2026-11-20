# Third-party notices

weemitt's own code is under the MIT license ([LICENSE](LICENSE)).

## The game

The game logic in `src/game/` and the other headers in `include/` are C from
[gloveworld](https://github.com/Eddie-Lack/gloveworld), the same author's decompilation of Glover (N64, USA), changed
for the PC. The repository holds no data from the game: the program reads the player's own ROM at start-up. Glover
belongs to its rights holders. The MIT license covers what the weemitt contributors wrote and gives no right to the
game.

## The N64 system headers

`include/ultra64.h` and the headers in `include/PR/` were written for weemitt. They declare only the parts of the
N64 system interface that the port's code uses. They hold no comments, notices or other prose from the N64 SDK.
Their names, values and structure layouts follow the N64 interface.

## Dear ImGui

`third_party/imgui/` holds Dear ImGui 1.90.1 by Omar Cornut and contributors, unchanged, under the MIT license. Its
license text is [third_party/imgui/LICENSE.txt](third_party/imgui/LICENSE.txt), and
[third_party/imgui/README.md](third_party/imgui/README.md) names its source.

Dear ImGui includes three files by Sean Barrett, each under the license in its own header (public domain or MIT):
`imstb_rectpack.h`, `imstb_textedit.h` and `imstb_truetype.h`.

## Libraries used and not included

The program links SDL2 (zlib license). Its code is not in this repository.
