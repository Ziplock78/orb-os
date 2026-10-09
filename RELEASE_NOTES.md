# What changed

One section per released version, newest first, written for the person holding the Orb
rather than for whoever wrote the code. `tools/publish-firmware.sh` lifts the section
matching `FW_VERSION` into `manifest.json`, and Orb Studio prints it beside the Flash
button, so somebody deciding whether to update can read what they would be getting.

It refuses to publish a version with no section here, for the same reason it refuses a
version that was not bumped: an update nobody can read about is one people put off.

Rules for a section, all of them learned from what reads badly on that card:

- Say what it does for them, not what was edited. "The clock keeps the time it has" beats
  "fixed getLocalTime timeout handling".
- One line per change, no more than about four lines, no full stops needed at the end.
- Name the person who found it when somebody did. It is their fix as much as anybody's.
- No version numbers, no file names, no capability levels. The card already shows those.

---

## 2.17.00

- The Orb stops running out of memory. This is what sits behind the theme sends that stop
  part way, the install page that will not load, the page at theorb.local that stops
  answering, and the feeds that go quiet after a long uptime. Diagnosed by Greg Takacs,
  measured and fixed first by Techtobi83
- The second hand sweeps evenly instead of jerking its way around the dial, and on a
  ticking clock the minute hand no longer appears in two pieces or pushes the second hand
  out of step. Found and fixed by Greg Takacs
- A theme with a long name finds its own artwork, so a freshly installed theme no longer
  comes up missing its background with the hands off center. Reported by biker_trash_1340
- Animated backgrounds keep animating and hold every frame for the same length of time,
  and the radar sweep moves evenly

This is a new baseline: everything since the last published version, in one release.
Merged by CanadianAvenger, with most of the firmware work by Greg Takacs.

---

## 2.16.71

- The second hand on a sweeping clock no longer jerks its way around the dial. Its
  wake-up was timed from when the previous frame FINISHED rather than when it began, so
  every frame arrived early by the cost of drawing it, and every expensive frame — a
  background slice, the minute hand's creep — pushed the next few beats out of step.
  The wait is now measured from the wake itself and rounded up, a beat is only ever
  given up whole, the hand's own rate stops flapping between 28,800 and 14,400 bph
  mid-sweep, and the minute-hand refresh that had been stepping off the beat grid now
  keeps it like every other frame. /health gained sweep_jitter_ms and a freeze count,
  so the next argument about this is settled with measurements
- The minute hand on a ticking (non-sweeping) clock no longer appears in two pieces every
  few seconds, and no longer hiccups the second hand when it moves. Its cached picture
  renewed itself in eight slices spread over eight frames — a sweep's answer, wrong for a
  dial that draws once a second, where the half-renewed hand sat on the glass and the
  renewal never finished before it went stale again; doing it all in the frame that
  noticed instead held the second hand's step half a second behind its own click. The
  renewal now happens whole between ticks, on the loop's idle time, while the previous
  picture keeps the glass filled; sweeping and animated dials are unchanged
- Found in Greg's 2026-10-07 report of the sweeping second hand and his 2026-10-08 report
  of the Aviator dial's two-phase minute hand, and the fix that four earlier diagnoses
  could not reach

---

## 2.16.70

- A ticking second hand moves with its own click instead of a second behind it. The wait for
  the next second was rounded down, so the hand woke a fraction early, drew the second that
  had not arrived yet, and then sat out the one it missed. Found by Zion, who kept saying the
  sound and the hand did not match while I kept looking somewhere else

---

## 2.16.69

- The health page now says what a dial costs to draw, split into the background picture, the
  text, the hands and the glass. A slow clock could only be diagnosed over a serial cable
  before this, so everybody who reported one was asked to describe it in words instead

---

## 2.16.68

- A ticking second hand moves at the same instant as its own click, on a clock that also has
  a moving background. The hand could land up to half a second after the sound it was
  supposed to arrive with. Found by Zion, who heard it the moment he turned the sweep off

---

## 2.16.67

- A looping background holds every frame for the same length of time. The picture's rate and
  the dial's drawing rate were unrelated numbers, so one frame stayed up for half as long as
  the next, for ever, however even the animation itself was. Found by Zion on a gear whose own
  frames are identical to a tenth of a percent

---

## 2.16.66

- Animated backgrounds keep animating. Theme art was not being released as it was loaded,
  so after a while there was no room left and the animation quietly stopped
- A theme install that finishes now says it finished, instead of reporting itself interrupted
- An animated background and moving hands can now run at the same time, because the hands are
  held rather than redrawn from scratch every frame
- Three faults in the clock's drawing caches that only showed on unusual designs
- The health page reports where memory has actually gone, which is what made all of the above
  findable

Almost all of this is the work of Greg Takacs, reviewed and merged by CanadianAvenger.

---

## 2.16.65

- Ordinary memory traffic moves out of the small internal pool, which is the one everything
  else fails from when it runs short. This is the change behind the theme sends that stop
  part way, the install page that will not load, and the feeds that go quiet after a long
  uptime. Diagnosed by Greg Takacs, measured and fixed first by Techtobi83

---

## 2.16.64

- The hour hand creeps again on a clock with a sweeping second hand. Since 2.16.56 it was
  being redrawn once an hour, so it stood still and then jumped a whole division instead of
  moving with the minutes. Found and diagnosed by wizard.oz
- The release page no longer tells you to update over WiFi at an address that is not there,
  and it now carries what actually changed in the version instead of only how to flash it.
  Both raised by wizard.oz as well

---

## 2.16.63

- The Flight Tracker gives up on an address that has stopped answering, instead of trying it
  for as long as the Orb stays switched on. Until now a power cycle was the only thing that
  cleared one, which is what people kept finding

---

## 2.16.62

- The Flight Tracker stops losing polls to an address that was never real. A name lookup that
  failed could still report success and hand back 0.0.0.0, and that was kept and tried for the
  rest of the session, which is a good part of why "adsb.lol is not answering" came up as often
  as it did. Found by Techtobi83
- The weather radar, the weather map and the cloud imagery each stop building a TLS client they
  never use, which was taking internal memory away from the download it was meant to help.
  Also Techtobi83
- And a download that comes back empty now says so, with how much memory there was at the time,
  instead of failing in silence

---

## 2.16.61

- An install that gets cut off no longer leaves the Orb stuck. It used to answer "install in
  progress" to sync, to handover and to deleting a theme, for as long as it stayed switched
  on, so the next sync could never get anything onto it
- And it will not boot wearing a theme that is only half installed, which showed as the hands
  of a theme with nothing behind them. A theme being replaced now stops counting as installed
  until the last file of it has arrived

---

## 2.16.60

- A sweeping clock no longer leaves a band of the dial at the wrong brightness. Each time
  the minute hand crept, its own area lost the glass over it and anything the design draws
  above the second hand, and did not get them back until the whole dial was repainted.
  Found by Jean-Paul Stringaro

---

## 2.16.59

- Coming back to the clock from Settings, or from any other app, now shows the whole face
  straight away. A sweeping dial used to come back black and paint itself in behind the
  second hand, never reaching the corners. Found by Zion while changing the tick level

---

## 2.16.58

- A ticking clock now waits two seconds after its face appears and then fades in, so the
  first thing you hear is a clock already keeping steady time rather than one starting up

---

## 2.16.57

- The tick's level control now behaves the way hearing does. The bottom of the range used to
  do almost nothing; 10% is now only just audible, which is what it should always have meant

---

## 2.16.56

- A sweeping second hand no longer hesitates every few seconds. The dial used to be redrawn
  in full just to creep the minute hand; now that hand moves in place and the rest is left alone

---

## 2.16.55

- The Orb now says which part of redrawing its dial is slow, not just that it is

---

## 2.16.54

- The Orb now reports how often it has to redraw its whole dial, and how long that takes, so
  a sweep that hesitates can be measured instead of guessed at

---

## 2.16.53

- A theme's tick now starts at 20% rather than 30%, so a clock that arrives ticking is quiet
  enough to live with before you have touched anything

---

## 2.16.52

- A ticking clock no longer disturbs a sweeping second hand. Its tick was waking thirteen
  times a second to keep its timing; one is enough and the hand gets the rest

---

## 2.16.51

- The clock's tick keeps its own time now, instead of riding the drawing. It used to land up
  to a sixth of a second from where it belonged, which was heard as clicks dropping
- Found by Zion

---

## 2.16.50

- A theme's tick is now carried as whole seconds rather than single clicks, so a fast
  mechanism keeps the natural ring of each click instead of having it cut short

---

## 2.16.49

- A tick can now beat at the rates real movements actually run at: five a second for an
  18,000 beat watch, six for 21,600, eight for 28,800, as well as one, two and four
- Found by Zion, whose vintage pocket watch beats five times a second and had nowhere to land

---

## 2.16.48

- A theme's tick can now beat two or four times a second as well as once, so a recording made
  from a mechanical watch runs at the speed its movement actually ran at
- The face still moves once a second, because a watch beating four times does not move its
  hand four times

---

## 2.16.47

- The clock's tick now sounds at the same instant the second hand moves, instead of just
  after it
- Found by Zion

---

## 2.16.46

- Settings, Sound now has its own level for the clock's tick and for the hourly chime, each
  with an OFF position, so a theme that ticks can be made quiet or silent without muting the Orb
- Both start at 30%, low enough that a theme arriving with a tick is one you turn up rather
  than one you switch off

---

## 2.16.45

- A clock that does not sweep now moves its second hand exactly on the second. It used to
  step at whatever moment its timer happened to fall on, up to a second away
- Its tick now sounds just after the hand moves rather than just before, the way a real one does
- Found by Zion

---

## 2.16.44

- A theme's tick recordings now play in the order they were given, looping, rather than being
  shuffled. Six takes recorded off one clock are a passage, not a bag of samples

---

## 2.16.43

- A theme can now carry the sound of its own clock ticking. It holds several recordings of
  one real click and plays a different one each second, so it never settles into a loop
- Themes without them are silent, exactly as before

---

## 2.16.42

- The railway minute hand no longer stutters. Designs light enough to draw it move it
  smoothly; heavier ones click it over in one clean move instead of jerking twice
- Fixed a design whose minute hand sits above its second hand drawing that hand a fraction
  off its mark on every frame
- Found by Zion

---

## 2.16.41

- The railway minute hand's step is one smooth movement again. In the last update it moved
  most of the way in a single frame and then corrected itself, which looked like two jerks
- Found by Zion

---

## 2.16.40

- On a Swiss railway clock the minute hand now travels across to the next mark instead of
  appearing on it: a quick snap with a slight settle, the way a station clock does
- It moves while the second hand is already waiting at the top, so the sweep is untouched

---

## 2.16.39

- On a Swiss railway clock the minute hand now sits exactly on a minute mark and moves at the
  top of the minute, wherever the Orb was in the minute when you switched it on. Since the
  last update it could come to rest partway between two marks and step at the wrong moment
- Found by Jean-Paul Stringaro

---

## 2.16.38

- On a sweeping clock the minute hand now creeps the whole time, the way a mechanical watch
  does. It used to hold still and jump a whole division at the top of the minute, which is
  correct for a railway dial and wrong for every other design. Railway dials still step,
  because there the step is the point. Found by Jean-Paul Stringaro, who noticed the Orb and
  Orb Studio disagreeing

---

## 2.16.37

- Older and smaller SD cards work again. The Orb runs the card fast, and a card that could
  not keep up was reported as no card at all rather than simply being run slower. Found by
  Overcore, who lost a day to a 2 GB card that was never faulty

---

## 2.16.36

- A moving background actually moves. Its frames were being written to the card but never
  loaded, so the picture sat on its first frame however the theme was set

---

## 2.16.35

- A slow moving background no longer slows the second hand with it. A background set to
  change once a second, which is what a ticking gear train wants, was setting the whole
  clock to one frame a second and turning a sweeping hand into a ticking one

---

## 2.16.34

- A clock background can now be a moving picture. Choose an animated GIF for the background
  in Orb Studio and say how it should play: held still and set going every so often, or
  running without stopping
- Held is the one to pick. Between plays it costs nothing at all and the second hand stays
  exactly as smooth as it is now, where a background that never stops slows it by about a
  third for as long as it runs
- Themes made this way still look right on an Orb that has not updated: it shows the first
  frame, standing still

---

## 2.16.33

- Your Orb now always shows the real sky. It could be told to invent aircraft by a theme,
  which meant a theme you installed from somebody else could fill your scope with traffic
  that was never there
- Orb Studio keeps its Test traffic switch for designing with, and it stays in the browser
- Found by Fly4Funn, whose Orb was showing eight aircraft he never asked for

## 2.16.32

- Headlines set at an angle now actually appear. They were being drawn nowhere at all, while
  still reacting to a tap, which is why a story would open if you guessed where one was
- The app switcher shows the names either side again, instead of only the one you are on.
  They were appearing only if you happened to give the centre name a glow
- Both found by Drewzy while building a full theme

## 2.16.31

- Settings, Location now shows the name of where your Orb is set, above the coordinates,
  so you can check it at a glance instead of reading numbers
- An Orb that was given bare coordinates and never told what they mean shows nothing there,
  rather than a blank line
- The other half of Lerxtwood's request, after the flight tracker line in the last update

## 2.16.30

- Your flight tracker can show the name of the place it is centred on, so the scope says
  Leeds, Utah rather than leaving you to read coordinates
- Switch it on in Orb Studio under Flight tracker, Location line: it is off until you ask,
  and it has every control the other text boxes have, including its own typeface and ALL CAPS
- Asked for by Lerxtwood, who had already built it in his own copy of the firmware

## 2.16.29

- City search keys do their own jobs again. Yesterday's comma landed one place along from
  where the keyboard expected it, so pressing comma backspaced, backspace typed a space, and
  space did nothing. Found by Lerxtwood the same day he got the comma he asked for

## 2.16.28

- ALL CAPS is now a switch on every line of text a theme draws: both clock banners, the
  Flight Tracker's and the Weather map's readouts, the app menu, and every line on the
  splash screen including the firmware version and the network address, which nobody could
  reach before. Asked for by Zion
- The Swiss railway stop is back, with a note in Orb Studio explaining what it is. The
  second hand goes round in 58.5 seconds and waits at 12, the way a station clock does. It
  was never the cause of the hands flashing to twelve; that was fixed separately in 2.16.26.
  Asked for by WizardOfOz and Lerxtwood

## 2.16.27

- Orb Studio can now tell your Orb where it is, using the computer it is plugged into. Your
  laptop knows the spot better than your internet connection does, and it knows your time
  zone too. Asked for by clock and CanadianAvenger
- The city search keyboard has a comma, so "LEEDS, UT" finds Leeds in Utah rather than four
  other Leeds. Lerxtwood's own fix

## 2.16.26

- The clock keeps the time it has. Hands were snapping to twelve for a moment at random, on
  every theme, and a digital face would blank for about a second. Found by Lerxtwood,
  CanadianAvenger and Drewzy between them
- The Swiss railway stop is gone. It was never the cause of the above, but it was one day
  old and not worth the confusion. A second hand now always shows the second it is

## 2.16.25

- A second hand could pause at twelve the way a Swiss station clock does. Removed again in
  2.16.26

## 2.16.24

- Text on the clock no longer has a faint seam across its background. Found by canoejohn

## 2.16.23

- Orb Studio can now say which of a theme's typefaces actually loaded, so a theme drawing
  the wrong font can be diagnosed rather than guessed at. Found by canoejohn
