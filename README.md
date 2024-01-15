Sweet Tea is part of a family of City of Heroes launchers (named after drinks
for some reason) that are compatible with the original Tequila launcher
manifests. This launcher was designed to work out of the box with no
dependencies while being more secure and convenient than Tequila.

[Tequila](https://github.com/Thunderspies/Tequila), the City of Heroes game
launcher written by the Secret Cabal of Reverse Engineers (SCoRE), was last
released in 2016. Its version of SSL was deprecated a year before its final
release, making it vulnerable to man-in-the-middle attacks. This is especially
concerning for a launcher that downloads and executes files. Additionally,
Tequila does not prevent a manifest from pointing to an absolute file path,
which makes it possible delete, modify, or create files anywhere on the system.
These security concerns inspired the development of a new launcher.

# Features for Players

* More secure than Tequila
* Parallel downloads
* Add your own City of Heroes manifests
* View all City of Heroes servers on one convenient list
* Provide additional launch parameters for running the game

# Features for Server Owners

* Compatible with Tequila XML manifests
* Display your icon on the server list
* Use a Message of The Day URL that can point to a static file or API endpoint

# Compiling

With Qt5 installed, simply use `qmake`.
