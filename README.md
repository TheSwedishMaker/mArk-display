# mArk — Family Task Display

mArk is a always-on home display that turns your family's daily calendar into something you can all see, track, and compete over. Each family member gets their own task list pulled live from their calendar. As tasks get marked done, a LED strip fills up. Finish everything and the screen celebrates. Every completed day adds to your monthly highscore — and being away a day never erases what you already earned.

Built on a 10" touchscreen, mArk lives on the kitchen counter or wherever the family gathers. No app to open, no notifications to dismiss. It's just there.

- Up to 6 family members, each with their own tasks and scores
- Tasks pulled automatically from Google Calendar or Apple Calendar
- Manual tasks from any phone — plan a week ahead, or make a task repeat weekly
- LED strip shows how much of the day is done
- Monthly highscore: days completed this month, fresh competition every month
- Levels (Starter → Consistent → Dedicated → Unstoppable → Legend → Titan) built on your total completed days — they never reset
- Recurring calendar events (daily/weekly/monthly/yearly) from ICS feeds, including removed single occurrences
- Countdown timers: add `[T]` to a calendar event and the task gets a built-in timer with an alarm
- Challenge medals: tag a task `[Name:N]` and earn a gold medal every N completions
- Rotary encoder for scrolling through tasks without touching the screen
- Web interface for managing calendars and tasks from any phone — no computer needed
- Wireless firmware updates from the browser after the first install
- English or Swedish display language (compile-time option in `secrets.h`)
- Soft power button: short press to turn the display and LEDs on/off

---

## What You Need

### Hardware

| Part | Details |
|---|---|
| Display | Elecrow CrowPanel Advanced 10.1" (ESP32-P4) |
| LED strip | WS2812 addressable LEDs (32 LEDs recommended) |
| Rotary encoder | Any standard KY-040 style encoder with push button |
| Power button | Any momentary push button |

The CrowPanel has WiFi, touch, and the main processor all built in. You just need to wire up the LEDs, encoder, and power button.

See `COMPONENTS.md` for exact parts with purchase links.

### Wiring

| Part | Signal | Connect to |
|---|---|---|
| LED strip | DIN (data) | IO2 |
| LED strip | VCC | 5V |
| LED strip | GND | GND |
| Power button | One leg | IO3 |
| Power button | Other leg | GND |
| Rotary encoder | CLK | IO27 |
| Rotary encoder | DT | IO28 |
| Rotary encoder | SW (push) | IO5 |
| Rotary encoder | VCC | 3.3V |
| Rotary encoder | GND | GND |

---

## Setup Guide

### Step 1: Install the build tools

mArk runs on ESP-IDF, the official development framework for the ESP32 chip. Think of it as the compiler and flasher that turns the code into something the display can run.

**Install ESP-IDF v5.4.2** by following the guide for your operating system:
https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/

The guide walks you through the full installation. Once done, open a new terminal and verify it worked:

```bash
idf.py --version
# Should print: ESP-IDF v5.4.2
```

If that command is not found, the installer did not finish correctly — re-run it and follow the step that says to run `export.sh` (Mac/Linux) or `export.bat` (Windows) to activate the environment.

---

### Step 2: Get the display drivers

The code that makes the screen actually work is provided separately by Elecrow (the company that makes the display). You need to copy it into this project.

```bash
# Download Elecrow's example code (you only need to do this once)
git clone https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen.git elecrow-examples

# Copy the drivers into this project
cp -r elecrow-examples/example/V1.0/idf-code/Lesson09-LVGL_Lighting_Control/peripheral esp32-project/
```

> **Important:** Use Lesson 09 specifically. Other lessons won't work.

If you see a white or blank screen after flashing, this step was missed or the wrong lesson was used.

---

### Step 3: Connect your calendar

mArk shows tasks that come from your calendar. There are two ways to connect it — you only need one, but you can use both.

#### Option A: ICS link (works with any calendar — recommended for most people)

Every major calendar app can generate a private link that mArk can read directly. This works with Google Calendar, Apple Calendar, Outlook, and most others. You don't need any API key.

**Google Calendar:**
1. Open Google Calendar on your computer
2. Click the three dots next to the calendar you want → **Settings and sharing**
3. Scroll down to **Secret address in iCal format**
4. Copy that URL — you'll paste it into mArk's web interface later (Step 7)

**Apple Calendar (iCloud):**
1. On Mac: open Calendar → right-click the calendar → **Share Calendar** → copy the link
2. On iPhone: Settings → your name → iCloud → make sure Calendars is turned on, then use the Mac steps above

#### Option B: Google API key (only needed for public Google Calendars)

If the calendar you want to use is set to **public** in Google Calendar, you can use this method instead. If you're not sure, use Option A — it always works regardless of privacy settings.

1. Go to https://console.cloud.google.com
2. Create a new project
3. Go to **APIs & Services → Library**, search for **Google Calendar API**, click **Enable**
4. Go to **APIs & Services → Credentials**, click **Create Credentials → API key**
5. Copy the key — you'll need it in the next step

---

### Step 4: Enter your WiFi and API key

Open the file `main/secrets.h.example`, make a copy of it called `main/secrets.h`, and fill in your details:

```bash
cp main/secrets.h.example main/secrets.h
```

Then open `main/secrets.h` in any text editor and fill in your values:

```c
#define WIFI_SSID      "your_wifi_network_name"
#define WIFI_PASSWORD  "your_wifi_password"
#define GCAL_API_KEY   "your_google_api_key"   // leave blank if using Option A only
```

This file is never uploaded to GitHub — your passwords stay on your computer only.

> **Time zone:** mArk automatically sets its clock from the internet when it connects to WiFi. It is currently set to Central European Time (CET/CEST). If you are in a different time zone, open `main/calendar_fetch.c` and find the line that says `"CET-1CEST,M3.5.0/2,M10.5.0/3"` — replace it with your own time zone string. A full list is available at https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

---

### Step 5: Build and flash

Connect the CrowPanel to your computer with a USB cable, then run:

```bash
cd esp32-project

# First time only — tells the build system which chip you're using
idf.py set-target esp32p4

# Compile the code
idf.py build

# Send it to the display (replace the port with yours — see below)
idf.py -p /dev/cu.wchusbserial10 flash
```

**Finding your port:**

On Mac, open Terminal and run `ls /dev/cu.*` — look for something with "usb" or "wch" in the name.

On Linux, run `ls /dev/ttyUSB* /dev/ttyACM*`.

On Windows, open Device Manager and look under "Ports (COM & LPT)".

The first build takes a few minutes. After that it's much faster.

> **Shortcut:** the file `flash.sh` in the project folder does the build, flash, and log viewer all in one command. Edit it first if your USB port name is different.

---

### Step 6: First boot

After flashing, the display restarts automatically and:

1. Connects to your WiFi
2. Sets its clock from the internet
3. Shows "Connecting..." while it fetches your calendar
4. Displays today's tasks

Once it's running, the display gets its own address on your home network. You'll use this address to manage calendars from your phone in the next step.

---

### Step 7: Add your calendar via the web interface

Open any browser on your phone or computer (while on the same WiFi as mArk) and go to:

```
http://taskviewer.local/
```

If that doesn't work, look at the display's startup screen — it shows the IP address (something like `192.168.1.42`). Use that instead:

```
http://192.168.1.42/
```

From here you can add calendars for each family member. Paste in the ICS link from Step 3 (Option A) or enter your Google Calendar ID if you're using the API key (Option B).

You can add up to 5 calendar sources per person, mix Google and ICS sources, and toggle individual calendars on or off.

The same page has a task panel where you can add manual tasks for each person — up to a week ahead. Tick **Every ...** on a task to make it repeat weekly on that weekday.

---

### Step 8: Add family members

mArk supports up to 6 people, each with their own task list, streak, and progress bar.

To add someone:
1. Tap **Settings** on the display
2. Tap **Add user**
3. Type their name
4. Go back to the web interface (Step 7) to add their calendar

To switch between people on the display: tap the person icons in the left sidebar.

---

## How to use it

| Action | What it does |
|---|---|
| Tap a task | Mark it done (tap again to undo) |
| Rotate the knob | Scroll through tasks |
| Press the knob | Mark current task done |
| Tap **Refresh** | Fetch the latest tasks from your calendar right now |
| Tap **Settings** | Manage users and calendars |
| Tap a person icon | Switch to that person's task list |
| Short press power button | Turn display and LEDs on/off |

Tasks refresh automatically every 5 minutes. The display also refreshes when you wake it up.

---

## Calendar tags — extra features straight from your calendar

Add these tags to an event's title in your calendar; mArk picks them up and hides the tag from the displayed name.

**`[T]` — countdown timer.** The task gets a timer button whose duration equals the event's length (end time minus start time). Tap to start; pause, resume or stop it; when it reaches zero an alarm sounds and the display wakes. Example: `Workout [T]`.

**`[Name:N]` — challenge series.** Links the task to a named challenge: every N completions earns a gold medal, shown next to your name on the leaderboard. Medals are permanent. Each family member has their own progress per series. Examples: `Run [Running:30]`, `Meditate [Calm:100]`. Works in calendar events and in manual tasks added from the web page.

---

## Display language

The display is in English by default. For Swedish, uncomment this line in your local `main/secrets.h` and rebuild:

```c
#define UI_LANG_SV  1
```

All display strings live in `main/lang.h`.

---

## Monthly highscore and levels

Every day someone completes all their tasks adds one day to their **monthly count** — the number shown in the sidebar and on the leaderboard. The count starts fresh at the beginning of each month, so every month is a new competition. Missing a day (being away, sick, no tasks) never takes anything away.

**Levels** are based on the total number of days you have ever completed, and they never reset:

| Level | Total days completed |
|---|---|
| Starter | 0 |
| Consistent | 5 |
| Dedicated | 15 |
| Unstoppable | 30 |
| Legend | 50 |
| Titan | 100 |

> **Upgrading from an older version?** The scoring changed: mArk used to count consecutive days and reset to zero after a missed day. Existing streaks are migrated automatically — your old streak becomes the starting value for your total days, so nobody loses their level.

---

## Updating the firmware — no cables needed

After the first USB install, all future updates are wireless:

1. Build the new version: `idf.py build`
2. Open `http://taskviewer.local/update` on any device on the same WiFi (there is also a link at the bottom of the settings page)
3. Upload `build/task_viewer.bin` and wait — the display verifies the file, installs it, and restarts by itself

All users, tasks, scores and calendar settings are kept. A failed or interrupted upload leaves the running firmware untouched.

---

## Hardware details

### Pin assignment

| Function | GPIO |
|---|---|
| LED strip data | IO2 |
| Power button | IO3 |
| Encoder push button | IO5 |
| Encoder CLK | IO27 |
| Encoder DT | IO28 |
| Audio right | IO14 |
| Audio left | IO16 |

### Full component list

See `COMPONENTS.md`.

---

## Credits

Recurring-event support (RRULE/EXDATE), the countdown timer, the challenge medal system, completion persistence across reboots, and several bug fixes were contributed by **[Christoffer Sjösten (Chriffe)](https://github.com/Chriffe)** in the [mArk-neo](https://github.com/Chriffe/mArk-neo) fork — thank you! His commits carry his authorship in this repository's history.

---

## Troubleshooting

**White or blank screen after flashing**
→ The display drivers are missing. Go back to Step 2 and make sure you copied the `peripheral/` folder from Lesson 09 specifically.

**"idf.py not found" error**
→ The ESP-IDF environment is not active in your terminal. Close the terminal, open a new one, and run the `export.sh` (Mac/Linux) or `export.bat` (Windows) script from your ESP-IDF installation folder before trying again.

**Display won't connect to WiFi**
→ Double-check the network name and password in `main/secrets.h`. The network must be 2.4 GHz — the display does not support 5 GHz WiFi.

**No tasks showing / "No events"**
→ If using an ICS link: make sure you copied the full URL including `https://`. If using Google API: make sure the calendar is set to public and the API key has Google Calendar API enabled.

**Web interface not loading**
→ Your phone must be on the same WiFi network as the display. Try the IP address shown on the display instead of `mark.local`.

**Tasks from yesterday still showing in the morning**
→ Tap **Refresh** to force an immediate update. If it keeps happening, make sure the display stays powered overnight so the automatic midnight refresh can run.
