# Ooze prevention

This option will drop the temperature of the inactive extruders to prevent oozing.

## Temperature variation

Temperature difference to be applied when an extruder is not active. The value is not used when 'idle_temperature' in filament settings is set to non-zero value.

## Preheat time

To reduce the waiting time after tool change, Orca can preheat the next tool while the current tool is still in use. This setting specifies the time in seconds to preheat the next tool. Orca will insert a M104 command to preheat the tool in advance.

## Preheat steps

Insert multiple preheat commands (e.g. M104.1). Only useful for Prusa XL. For other printers, please set it to 1.

## Delta temperature

**Preheat delta temperature**: the offset from the target temperature used to preheat the next tool. With a target of 220 °C and a delta of -30, the tool preheats to 190 °C. Printers that preheat with M104.1 (Prusa XL) do not use it.
