# How it works

This is the longer version: what goes wrong, which pieces of Pimax's and Tobii's software are involved, and exactly what
the monitor looks at and does. I worked it out from the logs on my PC and from the installed Pimax Play files, so it's
my best understanding, not anything official from Pimax.

## The pieces involved

| Component | Where it lives | What it does |
|---|---|---|
| Pimax Play (desktop app) | `C:\Program Files\Pimax\PimaxClient` (an Electron app) | The settings window and tray app. Its "device-pump" reacts to the headset connecting and disconnecting. |
| PiService | Pimax's background service | Talks to the headset over USB, e.g. passes on the eye tracking switch. |
| launcher.exe | `C:\Program Files\Pimax\Runtime\launcher.exe` | A small command-line tool that passes a command to PiServiceLauncher over a named pipe. |
| PiServiceLauncher | Pimax's elevated service | Starts and stops other services, including the Tobii runtime. |
| Tobii Platform Runtime | `platform_runtime_VR4PIMAXP3B_service.exe`, logs in `C:\ProgramData\Tobii\Tobii Platform Runtime\XR5EYECHIP` | Tobii's service that opens the eye tracker (a USB device inside the headset) and serves gaze to clients. |
| pi_server | Pimax's runtime server, logs in `%LOCALAPPDATA%\Pimax\runtime\pvr_srv_log_*.txt` | Connects to the Tobii runtime and hands gaze to anything using Pimax's PVR SDK. |
| CustomHeadsetOpenVR | SteamVR driver by sboys3 | Reads gaze from the PVR SDK (`pvr_getEyeTrackingInfo`) and gives it to SteamVR as an eye tracking device. |
| SteamVR | | Passes gaze on to games, e.g. through `GetEyeTrackedFoveationCenter` or OpenXR's eye gaze extension. |

So gaze goes: eye tracker → Tobii runtime → pi_server → CustomHeadsetOpenVR → SteamVR → game.

## What goes wrong

1. The headset stays plugged in. Pimax Play starts with Windows, the Tobii runtime opens the eye tracker, pi_server
   connects to it, and eye tracking works.
2. I start SteamVR. When CustomHeadsetOpenVR starts the headset, Pimax Play logs "HMD disconnected" and then "HMD first
   connected". As far as Pimax Play is concerned, the headset has just been plugged in.
3. Three seconds after "HMD first connected", Pimax Play's device-pump runs `launchEyeService`. That runs
   `launcher.exe PiServiceLauncher sendCommand active_tobii_runtime`.
4. PiServiceLauncher removes the Tobii runtime service and installs and starts it again (`remove`, then
   `installstart`).
5. The new Tobii runtime tries to open the eye tracker. Sometimes the old instance hasn't let go of the USB device yet,
   and it gets `Could not create device handle, ACCESS_DENIED`.
6. pi_server loses its connection (`tobii_device_process_callbacks failed`) and tries `tobii_device_reconnect` 6 or 7
   times in under 1.5 seconds. If the runtime still can't open the tracker by then, pi_server logs `tobi api failed`
   and never tries again.
7. From then on, the PVR SDK returns no gaze, the driver has nothing to pass on, and SteamVR has no gaze for the rest of
   the session. SteamVR also keeps returning the last gaze it had as valid, so a game sees the gaze freeze rather than
   disappear.

It's a race. On my PC it was lost roughly a third of the time. On the other starts the new runtime opened the tracker
in time and everything carried on.

An example from 27 September, with times from the different logs:

```
13:43:54  vrserver startup
13:44:00  CustomHeadsetOpenVR: Starting eye tracking thread
13:44:18  (Pimax Play) HMD first connected
13:44:21  (Tobii service) Received quit event -> stopped -> re-registered
13:44:23  (Tobii runtime) Could not create device handle, ACCESS_DENIED
13:44:23  (pi_server) tobii_device_process_callbacks failed; try tobii_device_reconnect ...
13:44:25  (pi_server) tobi api failed      <- no eye tracking until it's restarted
```

I reported it to Pimax and on the CustomHeadsetOpenVR tracker
([#115](https://github.com/sboys3/CustomHeadsetOpenVR/issues/115)). Neither could reproduce it. Updating to Pimax Play
2.0.4 with a clean install didn't change it, and the device-pump code is the same there. Quitting Pimax Play isn't a
workaround either: that stops pi_server, and the driver needs Pimax Play running.

## Why turning eye tracking off and on fixes it

Pimax Play's eye tracking switch does two things when it goes on:

1. It tells the headset (through PiService) to switch the eye tracker on. Off and then on power-cycles the tracker's USB
   device.
2. It runs the same `active_tobii_runtime` command as above, so the Tobii runtime starts again and pi_server reconnects
   to it (`Connected to platform module`).

By the time you do this, nothing else is restarting, so the race is usually won. It's still the same race, though, so
occasionally it takes two goes.

## What the monitor does

### Reading the state

The monitor reads only what has been added to each log since last time: Pimax's logs twice a second, the driver's `diagnostic.json` four times a second, and SteamVR about 60 times a second.

| Log line (pi_server) | Monitor state |
|---|---|
| `Eye calibration applied`, or `tobii_gaze: <rate>` with a rate above 1 | Connected |
| `tobii_device_process_callbacks failed` | Lost, Pimax reconnecting |
| `tobi api failed` or `tobii_calibration_apply failed` | Gave up |
| `Connected to platform module` after Lost or Gave up | Reconnected |
| `receive quit event to exit` | Pimax service stopped |
| `User eye detected info changed to 0/1` | Eyes detected or not (used as "worn") |

A new `pvr_srv_log` file means pi_server restarted, and the state starts again from unknown.

The SteamVR gaze counts as frozen when it hasn't changed for 1 second. A real eye never holds perfectly still, and
SteamVR keeps returning the last gaze as valid after the tracker stops.

### Deciding when to restart

The trigger is only the "gave up" state. Before that, Pimax is still trying, and restarting in the middle of that could
make things worse. After `tobi api failed`, nothing is going to try again by itself, so a restart can't break anything.

Then:

1. Wait 5 seconds after "gave up" is first seen.
2. Wait until Pimax's launcher log (`%LOCALAPPDATA%\Pimax\PiService\launcher_*.log`) hasn't been written to for 12
   seconds. The launcher logs every command it passes on, so this means Pimax Play isn't in the middle of a restart of
   its own.
3. Read `piplay_enable_eye_tracking`. If it's 0, you switched eye tracking off in Pimax Play, which is why pi_server
   let go, so it stays off and the monitor goes yellow.
4. Restart: switch off, wait 5 seconds, switch on, then run `active_tobii_runtime`.
5. If pi_server is back to Connected, reset. If it's still "gave up" 20 seconds after the last attempt, try again. After
   3 attempts, stop and go red until you restart it yourself.

### Restarting

The restart uses the same calls Pimax Play makes. Pimax Play's window loads `PiPlayService.dll` and calls it directly:

```
PiServiceInit()
PiService_GetIntConfig("piplay_enable_eye_tracking", &was)
PiService_SetIntConfig("piplay_enable_eye_tracking", 0)
  ... 5 seconds ...
PiService_SetIntConfig("piplay_enable_eye_tracking", 1)
launcher.exe PiServiceLauncher sendCommand active_tobii_runtime
```

On the Dream Air SE, Pimax Play also passes a K10 config file with that last command. The Dream Air takes none, and the
monitor doesn't pass one. If you have a Dream Air SE and the restart doesn't bring the tracker back, please open an
issue.

### Worn or not

The colours depend on whether you're wearing the headset, so that taking it off isn't shown as a fault. The monitor
uses, in order:

1. the eye tracker seeing eyes (pi_server's `User eye detected` line), whenever Pimax has reported it;
2. Pimax Play's "mounted" flag from its service log. Pimax told me this is about OSC availability, not a proximity
   sensor (the Dream Air has none), and it can stay stuck after a service restart, so it's only a fallback;
3. SteamVR's activity level, if CustomHeadsetOpenVR is set to use the hardware proximity sensor
   (`proximitySensorType` 1);
4. otherwise, head movement: worn if the headset turned faster than 1.8°/s in the last 3 seconds.

### In the headset

The monitor connects to SteamVR as an overlay application once SteamVR is running. It uses SteamVR's own
`openvr_api.dll`, found through `%LOCALAPPDATA%\openvr\openvrpaths.vrpath`. It creates the "Eye tracking" dashboard tab
and the badge as overlays.

Overlay images are only sent when they change, and the dashboard panel only while the dashboard is open. SteamVR has
a limit of about 200 outstanding image uploads, and resending 4 times a second with the dashboard closed ran into it
and left the tab empty.

The monitor deliberately doesn't create a Direct3D device. An earlier version that did made a game crash at startup in
the NVIDIA driver.
