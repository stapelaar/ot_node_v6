# Node command reference — NDxx/IN/CMD

Elke node pollt aan het eind van iedere meetcyclus via CoAP GET op
`NDxx/IN/CMD`. Staat er een commando klaar (gepubliceerd via de MQTT broker),
dan wordt het uitgevoerd en volgt een ACK op `NDxx/OUT/CMD/ACK`.

Publiceren:
```bash
mosquitto_pub -h 192.168.1.5 -u <user> -P <pass> -t "NDxx/IN/CMD" -m "<commando>"
```
(elke MQTT-client werkt, ook tegen RabbitMQ's MQTT-plugin)

Reactietijd: max. één meetcyclus (dus max. het huidige interval van de node).

## Commando's

### `RESET`
Soft reset van de node (`sys_reboot()`).

- **ACK:** `OK:RESET`
- Node herstart binnen ~200ms na de ACK.
- Na boot publiceert de node automatisch `SYS/INTERVAL` en `SYS/FW`
  (zie firmware-versioning.md) — dus een RESET is meteen ook een manier
  om de actuele instellingen en firmwareversie op te vragen.

### `INTERVAL:<seconden>`
Wijzigt het meetinterval, persistent opgeslagen in ZMS (overleeft reboot).
Geldig bereik: 1–86400 seconden.

- **ACK bij succes:** `OK:INTERVAL:<seconden>`
- **ACK bij ongeldige waarde:** `ERR:INTERVAL:INVALID`
- **ACK op vergrendelde node:** `ERR:INTERVAL:LOCKED`
  (zie hieronder — `CONFIG_APP_INTERVAL_LOCKED`)
- Publiceert direct ook op `SYS/INTERVAL` zodat OpenHAB in sync blijft.
- Effect gaat in na de lopende cyclus (dus max. 1 cyclus vertraging op de
  oude cadans, daarna geldt het nieuwe interval).

**Vergrendelde nodes** — cadans is hier functioneel, niet cosmetisch:
pulstellers (watermeter, en straks regenmeter) rekenen op een vaste
tijdbasis voor hun LITERS_DELTA-achtige berekeningen. Op deze nodes staat
`CONFIG_APP_INTERVAL_LOCKED=y` (bij de watermeter automatisch via Kconfig
`select`), en wordt elk INTERVAL-commando geweigerd:

| Node | Reden |
|---|---|
| ND70  | badkamer beneden — vaste cadans voor vochtdetectie |
| ND142 | watermeter — pulsteller, cadans = tijdbasis van de berekening |
| ND210 | douche boven — vaste cadans voor vochtdetectie |

### `CALIBRATE` / `CALIBRATE:<ppm>`
Alleen op nodes met `CONFIG_APP_USE_SCD41_SENSOR=y`. Forced recalibration
van de SCD41 CO2-sensor.

- Zonder argument: kalibreert op 420 ppm (buitenlucht-referentie).
- Met argument: kalibreert op de opgegeven ppm-waarde (geldig: 400–2000).
- **ACK bij succes:** `OK:CALIBRATE:<ppm>`
- **ACK bij ongeldige ppm:** `ERR:CALIBRATE:PPM_RANGE`
- **ACK bij fout:** `ERR:CALIBRATE:<foutcode>`
- **Uitgesteld tijdens warmup:** als de SCD41 nog opwarmt, wordt het
  commando bewaard en automatisch herhaald in de volgende cyclus —
  geen ACK totdat het echt is uitgevoerd (of alsnog faalt).

### Onbekend commando
Elke payload die geen van bovenstaande patronen matcht:

- **ACK:** `ERR:UNKNOWN:<payload>`

## Topics — overzicht

| Topic | Richting | Inhoud |
|---|---|---|
| `NDxx/IN/CMD` | OH/broker → node | Commando (gepolld door de node) |
| `NDxx/OUT/CMD/ACK` | node → OH | Resultaat van het laatst uitgevoerde commando |
| `NDxx/OUT/SYS/INTERVAL` | node → OH | Actueel meetinterval; gepubliceerd na boot/reset én na wijziging |
| `NDxx/OUT/SYS/FW` | node → OH | `<versie>+<githash>[-dirty] <builddatum>`; gepubliceerd na boot/reset |

## Uitbreiden

Nieuw commando toevoegen in `node_cmd.c` → `dispatch()`:
1. Nieuwe `if (strcmp/strncmp(cmd, "...") == 0)` tak toevoegen.
2. ACK'en via de bestaande `ack(root, "...")` helper.
3. Instellingen die reboot moeten overleven: eigen ZMS-key in
   `app_settings.c` (zie `ZMS_ID_INTERVAL_S` als voorbeeld).
4. Node-specifieke commando's (zoals CALIBRATE) achter een
   `#if IS_ENABLED(CONFIG_APP_USE_...)` zetten, zodat ze alleen compileren
   op nodes die de betreffende sensor hebben.
