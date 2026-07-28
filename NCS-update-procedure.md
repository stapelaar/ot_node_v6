# NCS SDK update procedure (in-place)

Werkwijze zoals gebruikt bij de update v3.2.4 → v3.3.1 (juli 2026).
Workspace: `~/ncs/v3.2.4/` (mapnaam blijft, inhoud wijzigt — let op verwarring).

## 0. Vooraf

- **Commit eerst het project**: `cd ~/ncs/v3.2.4/ot_node_v6 && git status` — geen
  uncommitted wijzigingen laten staan.
  
cd ~/ncs/v3.2.4/ot_node_v6  
git add -A
git commit -m "voor de ncs update van 3.2.4 naar 3.3.1"
git push
  
  
- Productienodes blijven op hun huidige firmware draaien; herflash pas na
  volledige validatie op de testbank.

## 1. Beschikbare versies checken (gewone terminal, NIET de ncs shell)

De nrfutil in de ncs-shell mist `sdk-manager`. Gebruik een verse terminal.


nrfutil upgrade            # update alle nrfutil commando's
nrfutil self-upgrade       # update nrfutil zelf
nrfutil sdk-manager toolchain search


Kies de nieuwste versie **waarvoor een toolchain bestaat**. Een SDK-tag kan
bestaan zonder toolchain (v3.4.0 had er geen — toen werd het v3.3.1).

## 2. Toolchain installeren (gewone terminal)


nrfutil sdk-manager toolchain install --ncs-version v3.3.1


## 3. SDK omzetten

Let op: in de **nrf** map, niet in het projectmap! De git stappen werken in
elke terminal, maar `west` bestaat alleen binnen de ncs-shell — start dus
eerst `./ncs-start.sh` (de oude toolchain-versie is prima; west volgt de
manifest).


cd ~/ncs/v3.2.4/nrf        # de sdk-nrf manifest repo
git fetch origin
git checkout v3.3.1
west update                # VERPLICHT na elke checkout — duurt lang (GB's)


Valkuilen:
- `git fetch` in `ot_node_v6` doen = je eigen project fetchen, niet de SDK.
- `west update` vergeten = manifest en modules lopen uiteen (zephyr blijft oud).
- `west: command not found` = je zit in een gewone terminal; start de ncs-shell.

## 4. Startscript aanpassen

In `~/ncs-start.sh` de versie aanpassen:


nrfutil sdk-manager toolchain launch --ncs-version v3.3.1 --shell


## 5. Nieuwe shell + Python requirements


./ncs-start.sh             # prompt toont nu (v3.3.1)
pip install -r ~/ncs/v3.2.4/zephyr/scripts/requirements.txt


## 6. Valideren op testnode


cd ~/ncs/v3.2.4/ot_node_v6
./build-xiao.sh --node NDxxx     # ALTIJD pristine na een SDK update


- Nooit `--no-pristine` gebruiken direct na een SDK-wissel of overlay-wijziging:
  de gecachte CMake config hergebruikt oude DTC overlay lijsten.
- Verwachte breekpunten bij een Zephyr sprong: hernoemde Kconfig symbolen,
  gewijzigde DT bindings/nodelabels in de board files (voorbeeld: nieuwe
  XIAO board files enabelden de onboard IMU `lsm6ds3tr_c` → disabled in
  eigen board overlay), gedrag rond drivers (bmp388/bmp390 compatible).
- Flash, join, en controleer alle sensoren en de CoAP publish keten.

## 7. Afronden

- Project committen: "NCS vX.Y.Z migration: ..."

cd ~/ncs/v3.2.4/ot_node_v6  
git add -A
git commit -m "NA de ncs update van 3.2.4 naar 3.3.1"
git push

- In README noteren welke NCS versie minimaal vereist is.
- Resterende node-types pas herbouwen/herflashen wanneer nodig, één voor één.

## Referentie

Officiële docs: https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/installation/updating.html

