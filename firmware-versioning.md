# Firmware versiebeheer — ot_node_v6

Waarom dit bestaat: een build-datum alleen vertelt *wanneer* er gecompileerd
is, niet *welke code* erin zit. Een oude branch vandaag herbouwd krijgt een
verse datum maar bevat oude functionaliteit. Dit systeem geeft elke node een
ondubbelzinnige identiteit: een versienummer voor mensen + een git-hash die
onweerlegbaar naar de exacte broncode wijst.

## De twee onderdelen

**1. Versienummer (semver) — `VERSION` bestand in de app-root**

```
VERSION_MAJOR = 6
VERSION_MINOR = 1
PATCHLEVEL = 0
VERSION_TWEAK = 0
EXTRAVERSION =
```

Zephyr genereert hieruit automatisch `app_version.h` met het macro
`APP_VERSION_STRING` ("6.1.0"). **Bump het versienummer alleen hier** — nooit
hardcoded in een .c bestand.

Moet exact naast de top-level `CMakeLists.txt` staan (dus in `ot_node_v6/`,
niet in `app/`), anders wordt `app_version.h` niet gegenereerd en faalt de
build met `fatal error: app_version.h: No such file or directory`.

**2. Git commit hash — via `CMakeLists.txt`**

```cmake
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_HASH)
    set(GIT_HASH "nogit")
endif()
target_compile_definitions(app PRIVATE GIT_HASH="${GIT_HASH}")
```

`git describe --always --dirty` levert de korte commit-hash (bv. `353758f`),
met een `-dirty` achtervoegsel als er niet-gecommitte wijzigingen meegebouwd
zijn. Zo'n build is **niet reproduceerbaar** — `-dirty` is het signaal om
niet blind te vertrouwen op wat er precies in zit.

## Waar het zichtbaar wordt

**Boot-banner (`main.c`)**
```c
LOG_INF(" %s v%s+%s", APP_NAME, APP_VERSION_STRING, GIT_HASH);
```
→ console toont: `ot_node v6.1.0+353758f`

**MQTT, na elke boot/RESET (`app_core.c`, cycle 1)**
```
NDxx/OUT/SYS/FW        → "6.1.0+353758f Jul 28 2026"
NDxx/OUT/SYS/INTERVAL  → actueel meetinterval (uit ZMS, of Kconfig default)
```
Publiceert in de eerste cyclus na boot — dus binnen ~5-10s, niet pas na het
volle meetinterval. Bij een node op 10 minuten cadans zou je anders 10
minuten moeten wachten op een eerste teken van leven.

**Na een INTERVAL-wijziging via CMD (`node_cmd.c`)**
Direct na een geslaagde `INTERVAL:<sec>` wordt de nieuwe waarde ook op
`SYS/INTERVAL` gepubliceerd — OpenHAB blijft altijd sync zonder de ACK te
hoeven parsen.

## Gebruik in de praktijk

**Release maken:**
1. Wijzigingen committen (`-dirty` verdwijnt pas ná commit)
2. Bij een functionele wijziging: `VERSION` bumpen (minor/patch)
3. Pristine build (`./build-xiao.sh --node NDxx`) — check de CMake-regel
   `-- Firmware git hash: ...` in de output
4. Flashen

**Uitzoeken welke node welke firmware draait:**
Kijk op `NDxx/OUT/SYS/FW` in OpenHAB/persistence. De hash is direct te
herleiden:
```bash
git log 353758f -1
git show 353758f
```

**Bekende valkuil:** `-dirty` in de hash betekent dat de build niet exact
overeenkomt met een commit — je hebt lokale wijzigingen gebouwd zonder ze
vast te leggen. Voor productienodes: altijd eerst committen, dan builden.

## Bestanden die hierbij horen

| Bestand | Rol |
|---|---|
| `VERSION` | Bron van het versienummer (app-root) |
| `CMakeLists.txt` | Bepaalt git-hash bij configure, geeft door als `GIT_HASH` macro |
| `app/src/main.c` | Boot-banner met versie+hash |
| `app/src/app_core.c` | `publish_sys_info()` — eenmalige SYS/INTERVAL en SYS/FW announce na boot |
| `app/src/node_cmd.c` | Publiceert SYS/INTERVAL opnieuw na een geslaagde INTERVAL-wijziging |
