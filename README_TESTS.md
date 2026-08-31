# Firmware-Unit-Tests (host, C++11)

Beide sind host-seitige Unit-Tests und laufen ohne ESP32/GCC-Cross-Compiler,
nur mit `g++` (oder `clang++`) im System.

## Einzeiler (alle Tests bauen + ausführen)
```bash
cd firmware/test
g++ -std=c++11 -Wall -Wextra -Werror -O2 -I../src/nav test_nav_message.cpp -o test_nav_message && ./test_nav_message
g++ -std=c++11 -Wall -Wextra -Werror -O2 -I../src/map test_map_frame.cpp -o test_map_frame   && ./test_map_frame
```

## Oder einzeln
```bash
cd firmware/test
# NavData (JSON, protocol §2/§5)
g++ -std=c++11 -Wall -Wextra -Werror -O2 -I../src/nav test_nav_message.cpp -o test_nav_message
./test_nav_message

# MapData (binary, protocol §4)
g++ -std=c++11 -Wall -Wextra -Werror -O2 -I../src/map test_map_frame.cpp -o test_map_frame
./test_map_frame
```

## Ausgabe
Jeder Test druckt `PASS  <name>` bzw. `FAIL  <name>` Zeile für Zeile,
dazu am Ende: `N passed, M failed`. Exit-Code 0 = alles grün, 1 = mindestens ein Fehler.

## Binaries (werden nicht ins Repo geschickt, .gitignore)
    firmware/test/test_nav_message
    firmware/test/test_map_frame
