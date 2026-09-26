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
