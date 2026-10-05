# Launch refresh regression tests

These QtTest integration tests drive real launcher widgets against a loopback
HTTP server. Launching runs only a temporary shell script that writes a marker;
no game executable or public manifest is downloaded. Test settings and files are
isolated in a temporary directory. Run the launch-marker tests on Linux/macOS.

With Qt 5 development tools (Widgets, Network, XML, Concurrent and Test):

```sh
mkdir -p build-tests
cd build-tests
qmake ../tests/launch_refresh.pro
make -j2
QT_QPA_PLATFORM=offscreen ./tst_launchrefresh
```

Run the original stale-launch regression alone:

```sh
QT_QPA_PLATFORM=offscreen ./tst_launchrefresh unchangedLaunchFetchesFreshManifest
```

The contract is that every Launch fetches its selected manifest anew. If changed,
all required files must successfully validate/update before automatically
launching once. Refresh or update failures must never launch cached content.

The full suite includes a real 30-second manifest timeout, so a passing run takes
about 35 seconds. Coverage includes HTTP/local refresh, changed arguments and
payloads, blocked/ambiguous profiles, mirror fallback, validation retry, keyboard
selection, repeated clicks, Options reloads, late metadata replies, and window
destruction while requests are pending.

For optional AddressSanitizer/UndefinedBehaviorSanitizer coverage:

```sh
mkdir -p build-tests-asan
cd build-tests-asan
qmake ../tests/launch_refresh.pro CONFIG+=debug CONFIG-=release \
  'QMAKE_CXXFLAGS+=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  'QMAKE_LFLAGS+=-fsanitize=address,undefined'
make -j2
ASAN_OPTIONS=detect_leaks=0 QT_QPA_PLATFORM=offscreen ./tst_launchrefresh
```

Leak detection is disabled for the sanitizer run because it includes the host
Qt/platform libraries; address and undefined-behavior failures remain enabled.
