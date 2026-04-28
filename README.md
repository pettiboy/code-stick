# M5 Voice Stick

A wearable voice-pendant prototype on the M5StickC Plus2.

The Stick hangs portrait-up as a **necklace**, broadcasting a mood as live abstract art. Hold the side button to push-to-talk; the phone transcribes. The transcript can also nudge the mood — say something loving and the necklace blooms; say something stormy and it crackles.

## Two modes, one device

- **Necklace** (default): the Stick shows one of 8 abstract moods. Pick from the phone, cycle on the device, or let the transcript suggest one.
- **Push-to-talk**: hold Button A, speak, release. The phone uploads to OpenAI and sends the transcript back to the Stick. The transcript briefly takes over the screen, then the mood returns.

## Moods

| id | feel | art |
| ------ | ------ | ------ |
| `PULSE` | alive | concentric rings expanding outward |
| `BLOOM` | in love | radiating petals beating |
| `DRIFT` | calm | layered sine waves |
| `STATIC` | anxious | TV noise + scanlines |
| `STORM` | angry | lightning bolts flickering |
| `ORBIT` | curious | dots circling a core |
| `GRID` | focused | pulsing geometric grid |
| `PRISM` | party | rotating triangles |

While recording, the active mood reacts to your voice — louder peaks intensify the animation. Idle, it breathes on a slow sine.

## Controls

| Button | Action | Effect |
| ------ | ------ | ------ |
| **A** (front "M5") | hold | push-to-talk · capture while held |
| **A** | double-tap | hands-free lock · tap A again to stop |
| **B** (top side) | tap | cycle mood (broadcasts to phone) |
| **B** | double-tap | brightness (4 levels) |
| **B** | hold | toggle device-info overlay |

Transcripts and faults overlay the mood for ~6 s, then the art returns.

## States

```
Standby ──link──▶ Ready ──BtnA──▶ Recording ──release──▶ Uploading ──TEXT──▶ Transcript
   ▲                ▲                                                            │
   └──disconnect────┴────────────── overlay timeout ───────────────────────────┘
```

## Folders

- `hardware-code-stick/` — PlatformIO firmware for the M5StickC Plus2.
- `mobile-app/` — Expo React Native app for iOS and Android.

## OpenAI

The phone calls:

- `POST https://api.openai.com/v1/audio/transcriptions` · model `gpt-4o-transcribe` · WAV
- `POST https://api.openai.com/v1/chat/completions` · model `gpt-4.1-mini` (romanization fallback for non-Latin script)

The API key lives in `mobile-app/secrets.js` (gitignored). For anything shared with other users, replace the direct OpenAI call with your own backend.

## BLE Protocol

Service UUID: `3e7a0001-e33b-4e2f-9a85-f03e1d33c001`

- Audio notify characteristic: `3e7a0002-e33b-4e2f-9a85-f03e1d33c001`
- Control notify/write characteristic: `3e7a0003-e33b-4e2f-9a85-f03e1d33c001`

Control messages are UTF-8 lines ending in `\n`:

| Direction | Message | Meaning |
| ------ | ------ | ------ |
| Stick → phone | `START\n` | recording started |
| Stick → phone | `STOP\n` | recording stopped |
| Stick → phone | `MOOD:<id>\n` | user cycled mood on device |
| Phone → Stick | `STATE:<text>\n` | phone status (Ready/Recording/Transcribing) |
| Phone → Stick | `TEXT:<transcript>\n` | transcript ready |
| Phone → Stick | `MOOD:<id>\n` | set mood (from tile tap or transcript suggestion) |
| Phone → Stick | `ERR:<message>\n` | upload error |

Mood ids: `PULSE`, `BLOOM`, `DRIFT`, `STATIC`, `STORM`, `ORBIT`, `GRID`, `PRISM`.

## Flash The Stick

```sh
cd hardware-code-stick
pio run -t upload
pio device monitor
```

The firmware advertises as `M5VoiceStick`.

## Run The Phone App

BLE native modules don't run in Expo Go. Build a dev client:

```sh
cd mobile-app
echo "export const OPENAI_API_KEY = 'sk-...';" > secrets.js
npm install
npm run prebuild
npm run run:android   # or run:ios
```

Tap **connect** in the app, then pick a mood or hold Button A on the Stick to talk.
