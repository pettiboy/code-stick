# M5 Voice Stick

Cross-platform prototype for an M5StickC Plus2 push-to-talk transcriber.

## Flow

1. Hold Button A on the M5StickC Plus2.
2. Speak into the Stick.
3. The Stick streams 16 kHz, 16-bit mono PCM to the phone over BLE.
4. The phone app wraps the PCM as a WAV file and calls OpenAI `audio/transcriptions`.
5. The phone sends `TEXT:<transcript>` back over BLE.
6. The Stick displays the transcript.

## Folders

- `code/hardware code-stick/` - PlatformIO firmware for the M5StickC Plus2.
- `code/mobile-app/` - Expo React Native app for iOS and Android.

## OpenAI

The mobile app uses:

- Endpoint: `POST https://api.openai.com/v1/audio/transcriptions`
- Model: `gpt-4o-mini-transcribe`
- File type sent: WAV

For a prototype, the app stores the API key locally with `expo-secure-store`. For anything shared with other users, replace the direct OpenAI call with your own backend so your API key is never shipped to phones.

## BLE Protocol

Service UUID: `3e7a0001-e33b-4e2f-9a85-f03e1d33c001`

- Audio notify characteristic: `3e7a0002-e33b-4e2f-9a85-f03e1d33c001`
- Control notify/write characteristic: `3e7a0003-e33b-4e2f-9a85-f03e1d33c001`

Control messages are UTF-8 lines ending in `\n`:

- Stick to phone: `START\n`
- Stick to phone: `STOP\n`
- Phone to Stick: `STATE:<text>\n`
- Phone to Stick: `TEXT:<transcript>\n`
- Phone to Stick: `ERR:<message>\n`

## Flash The Stick

```sh
cd "code/hardware code-stick"
pio run -t upload
pio device monitor
```

The firmware advertises as `M5VoiceStick`.

## Run The Phone App

BLE native modules do not run inside Expo Go. Build a dev client or native app:

```sh
cd code/mobile-app
npm install
npm run prebuild
npm run run:android
```

For iOS:

```sh
cd code/mobile-app
npm install
npm run prebuild
npm run run:ios
```

Then enter your OpenAI API key, tap **Connect**, and hold Button A on the Stick while speaking.
