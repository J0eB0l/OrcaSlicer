# AFC lanes in Snapmaker Orca

Printers that run [AFC](https://github.com/ArmoredTurtle/AFC-Klipper-Add-On) on Klipper (Box Turtle, ACE, OpenAMS, Bambu AMS through BridgeBox, and any mix of them) show their lanes in the slicer, sync filaments from them and map project filaments to lanes before a print.

## What it does

- **Lanes panel on the Device tab.** A side panel next to the printer page lists every toolhead, every unit and every lane: colour, material, vendor, remaining grams, Spoolman spool, which T number it answers to and whether it is loaded in a toolhead. Clicking a lane edits its T number, material, colour, temperatures, weight or Spoolman spool and writes the change back to AFC. Lanes can be loaded, unloaded or ejected from the same popup.
- **Drying.** Units with a heater (ACE, ACE 2 Pro, Bambu AMS 2 Pro, AMS HT) show their temperature and humidity and a Dry button. The dry popup offers material presets, a temperature and time, and whether to turn the spools. A unit that is drying shows the target, the time left and a Stop button.
- **Filament sync.** The Sync filaments button fills one slicer slot per AFC T number, with the lane's colour, material and Spoolman data. A printer with 17 lanes gets 17 slots.
- **Print mapping.** Before a print starts, a popup suggests a lane for every project filament (by material, colour and remaining weight), lets each one be changed and applies the result to AFC with `SET_MAP`.
- **Flow K.** A lane whose Spoolman spool has a saved flow K (the pressure advance AFC autocal stores in the spool's `flow_k` field) shows it on the lane, in the lane editor and in the print popup. Sync filaments gives every slot the K of the spool it was synced from: that slot prints with it, with pressure advance turned on even if the filament preset has it off. Slots whose spool has no K keep the preset's value. Picking another filament preset for a slot drops its spool K, and the print popup notes a slot mapped to a spool whose K differs from the one it was sliced with. On printers running AFC autocal with `apply_stored_k`, AFC applies the loaded spool's K itself and ignores the slicer's.
- **Dark mode.** The panel and the print popup follow the slicer's dark mode. The U1 device page switches to Snapmaker's own dark theme too; Snapmaker has not finished it, so its camera and control cards stay light.

## Which printers

- **Snapmaker U1** with AFC: connect the printer on the Device tab as usual.
- **Any other Klipper printer** with AFC: set the printer preset's host type to Octo/Klipper, enter the Moonraker address (and API key if Moonraker asks for one) in the printer's connection settings. The Device tab shows the printer's own web page (Fluidd or Mainsail) with the AFC panel next to it.

The panel stays hidden on printers without AFC.

## Notes

- AFC lane T numbers do not need to be continuous. A gap (for example `T16` to `T19` unused, `T20` on a lane) shows as an empty slot in the sync.
- On printers other than the U1, the printer profile has to accept as many filaments as there are AFC T numbers, with AFC handling the `T` commands in the G-code (a single-extruder multi-material style profile).
- The panel page lives in `resources/web/afc/`. Opening `index.html?view=panel&mock=1` (or `view=print`) in a browser shows it with demo data modelled on a six-toolhead AFC printer, without the slicer.
