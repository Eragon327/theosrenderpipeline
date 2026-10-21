# Weather overrides

Weather overrides live in the **Neural Rendering** tab. An override changes any
Neural Rendering setting or sharpening for the weathers and places you choose,
with different values by time of day if you want, using Skyrim's weather and game
clock. Everywhere else uses **Base**, your normal settings. ENB Helper is not
required. Up to **512 overrides** and **4096 exact weather assignments** are
supported. Overrides are saved as preset files (see below); earlier versions
called them presets.

## Layout

The left column lists **Base** first, then your overrides. Each row says when it
applies and how many settings it changes, and **NOW** marks the rows your edited
settings select for the current weather. Once an override exists, a **Now** line
shows what is applied, the game time and weather; hover it for the resolved values.

The right column shows the selected row's settings: each label on the left, its
control on the right. Passes are folder tabs above one pass's settings: **+** adds
the next pass, up to Pass 5, and **×** on the last pass's tab removes it (its own
settings are kept for later).
Settings that do not apply are greyed out with the reason, never hidden. While Base
is selected, a cloud beside a setting marks one the override in use now replaces;
hover it for the override and its value.

Overrides apply whenever at least one is in use. There is no separate automation
switch. **Pause overrides** immediately uses Base until you untick it or restart the
game; it is not saved.

## Create and edit overrides

1. Tune Base. Sharpening applies with or without NR. Each extra pass follows Pass 1
   until you change one of its settings, which gives it its own settings starting
   from Pass 1's; **Reset to Pass 1**, attached to its tab, makes that pass follow
   Pass 1 again and leaves the other passes alone.
2. **New override** starts with no changes. **Duplicate** copies the selected
   override.
3. Choose when it applies:
   - **Weathers**: exact weathers as chips. **Add current** adds the weather you are
     in; **Add...** searches every loaded weather. Click a chip to remove it.
   - **Weather types**: tick Clear, Cloudy, Rain or Snow.
   - **Places**: tick **Interiors** or **Anywhere outside** (any outdoor weather no
     more specific override covers).
4. Change the settings you want different. A changed setting is amber and has an
   undo arrow; hover it for Base's value, click it to return to Base. Setting a value
   back to Base's also removes the change. **Reset all to Base** clears every change.
   Settings you leave alone follow Base, including later Base edits. Each pass's
   settings are marked separately, so an override can change only Pass 2's style.
5. **Apply** for this session or **Save as default** for future sessions.
   **Discard** restores the applied configuration.

Look settings (intensity, local tone, local structure and sharpening strength)
blend smoothly between weathers and times of day and keep NR history. **Time of
day** is a strip: **All day** gives one look; pick Night, Dawn, Sunrise, Day, Sunset
or Dusk to give that time its own look, which blends into the next. The current time
is green. Choosing All day again keeps the selected time's values. Other settings
switch once at the midpoint of a weather change and reset NR history.

Settings marked **!** restart NR briefly when they change: placement, NR input
resolution, network, peripheral compression, combined preparation and
Reconstruction. An override that changes one of them can hitch when the weather
changes; interior and exterior changes happen behind a loading screen. Pass count
switches without restarting NR: the most passes any override in use asks for stay
allocated, and situations with fewer skip the rest like the combat option.

Overrides never switch NR or sharpening on while Base has them off, but can switch
them off. The Image tab shows Base sharpening read-only, and the value in use when
an override changes it. Community Shaders owns sharpening on its route, so overrides
change only TRP NR there. **Use** in the override's header ignores it without
deleting it. **Timing...** holds the times of day and transition smoothing. See the
[focused game check](APPEARANCE-TESTING.md) before relying on a new set.

## Weather types, places and exact weathers

Several overrides can use the same weather type, place or exact weather; the one
highest in the list wins, and **Move up**/**Move down** change the
order. A claim that a higher override wins is shown muted; hover it to see which.

**Add weathers...** searches editor ID/name, owning plugin, FormID and weather
type. Names use the engine or the optional Native EditorID Fix and powerofthree's
Tweaks public lookup APIs when loaded. Neither plugin is required; a plugin/local
FormID label remains available otherwise. **Refresh list** resamples names on
Skyrim's main thread. The list includes loaded records, not just weathers that
normally occur in the current region. Select rows individually or use **Select
shown** after filtering; selection survives filtering. The list shows weathers
another override already uses. Capacity errors leave the batch unchanged.

Assignments for absent plugins remain saved as greyed chips; they do not match
another plugin with the same numeric ID. Deleting an override removes its assignments;
its weathers fall to the next override that uses them, or to less specific ones.

Outdoor resolution is Base, then Anywhere outside, then Clear/Cloudy/Rain/Snow, then
an exact weather, per setting: a more specific override replaces only the settings
it changes. The Now line names every override in use, most specific first. Fog, ash and
special-worldspace weather can be added as specific weathers; their visible
appearance is not reliably described by the game's broad classification alone.
Ordinary interiors use only Interiors over Base. Sky-lit interior cells follow
outdoor rules.

## Time and transitions

Default anchors are Night 00:00, Dawn 05:00, Sunrise 07:00, Day 12:00, Sunset 18:00
and Dusk 20:00. Values interpolate between adjacent anchors, including midnight.
Hours must be within a 24-hour day and increase in that order.

**Copy ENB timing as anchors** reads `enbseries.ini` beside the game executable
only when clicked. It copies NightTime, SunriseTime, DayTime and SunsetTime and
derives Dawn as SunriseTime minus DawnDuration, Dusk as SunsetTime plus DuskDuration.
For example, markers 1/8.5/10.5/17.5 with durations 3.5/4 give anchors
01:00/05:00/08:30/10:30/17:30/21:30. Review and Apply the result. Missing or
non-increasing schedules leave the previous anchors unchanged.

This provides an editable approximation from the ENB configuration. TRP retains
its own linear interpolation; it does not reproduce ENB's internal phase weights,
follow later ENB edits automatically or change ENB settings.

Both outgoing and incoming overrides are sampled at the current hour, then blended
using Skyrim's weather transition progress. Additional smoothing softens sudden
changes. Its seconds value is an exponential time constant, not a fixed completion
duration. Loading clears the old scene's blend. Zero smoothing follows the
weather/time result directly. Base values and authored overrides never receive
intermediate blended values.

## Override files and sharing

Each override is one file in `Data/SKSE/Plugins/TheosRenderPipeline/Presets/`, and
the file name is its name: `Moody rain.ini` is the override "Moody rain". Renaming the
file renames the override, and a copied file is a new override. Files added to that
folder load at the next game start. Names cannot contain characters Windows file
names forbid, such as `?` or `:`. Renaming an override in game renames its file; under
MO2 the renamed file is written to Overwrite.

Under Mod Organizer, overrides you create are written to Overwrite, like other
settings the game creates; saving an override that came from a mod updates that mod's
file. To package overrides as a mod:

1. In MO2, choose **Create empty mod** and name it, for example
   `TRP Weather Overrides - Moody`.
2. Open the new mod's folder and create `SKSE/Plugins/TheosRenderPipeline/Presets/`.
3. Copy the override `.ini` files you want from MO2's Overwrite (the same path) into
   that folder, and enable the mod.

Right-clicking Overwrite and choosing **Create mod...** does the same in one step
for everything Overwrite contains. The mod is a pack of overrides: upload it as an
ordinary Data mod, and players or modlists install and enable it like any other
mod. When two mods contain the same override file, MO2's mod order decides which one
the game uses.

An override stores only the settings it changes, so it looks different on a
different Base. **Save full copy** adds a copy with every setting stored, which
looks the same for anyone; use it for packs meant to be a complete look, with
Interiors and Anywhere outside ticked. An override file contains:

```ini
[Preset]
Format = 1
Enabled = true
UseWhen = Rain|Snow
WeatherCount = 1
Weather0 = Skyrim.esm|10A241

[Changes]
Pass2SameAsPass1 = 0
Pass2Style = 7

[Changes.Night]
Pass1Intensity = 1.2
```

`UseWhen` accepts Outdoors (shown as Anywhere outside), Clear, Cloudy, Rain, Snow
and Interior (shown as Interiors). Weathers are a
plugin name and its local FormID, so they work in any load order. `[Changes]` uses
the same keys as the Neural Rendering settings; look settings go in the six time
sections (Night, Dawn, Sunrise, Day, Sunset, Dusk). On/off settings accept 1/0 or
true/false. Pass 2 settings apply only while Pass 2 has its own settings,
`Pass2SameAsPass1 = 0`; the editor adds it when you change a Pass 2 setting.

## Persistence and compatibility

The saved settings (`Data/SKSE/Plugins/TheosRenderPipeline.User.ini`, over the
packaged `TheosRenderPipeline.ini`) keep `[Appearance]` format 4: the time
schedule, smoothing and `PresetOrder`, the list order by file name. Files it does
not list, such as newly added packs, follow in name order. `Enabled` is written as
whether any override is in use, for older builds; this build derives it on load.

Save as default writes every override file, renames the file of a renamed override
and deletes the files of deleted overrides; Discard undoes both before saving. Override
files the game has not loaded are left alone. Overrides
from formats 1-3 in the main INI load as before and move into files at the next
save, which removes their old sections. Unrelated INI settings remain intact.
Older plugin builds cannot read format 4 or preset files; keep a backup before
reverting the DLL.

Install the matching `RCAS.hlsl` with the DLL. Sharpening uses a runtime constant
buffer; unchanged strength does not upload it again. Weather identities and override
selection are cached across ordinary frames. Disabled automation samples the
context display at four updates per second.

Gradual look changes retain NR history. Manual/override edits, loading, camera
resets, time jumps and other setting changes still reset it. Standalone tests
do not establish NVIDIA temporal image quality, game performance or visual
acceptance of the new UI.
