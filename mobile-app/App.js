import { StatusBar } from 'expo-status-bar';
import * as SecureStore from 'expo-secure-store';
import { File, Paths } from 'expo-file-system';
import { Buffer } from 'buffer';
import { useEffect, useMemo, useReducer, useRef, useState } from 'react';
import {
  Alert,
  Animated,
  Easing,
  PermissionsAndroid,
  Platform,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { SafeAreaView, useSafeAreaInsets } from 'react-native-safe-area-context';
import { BleManager } from 'react-native-ble-plx';
import {
  useFonts as useMajorMono,
  MajorMonoDisplay_400Regular,
} from '@expo-google-fonts/major-mono-display';
import {
  JetBrainsMono_400Regular,
  JetBrainsMono_500Medium,
  JetBrainsMono_700Bold,
} from '@expo-google-fonts/jetbrains-mono';

const DEVICE_NAME = 'M5VoiceStick';
const SERVICE_UUID = '3e7a0001-e33b-4e2f-9a85-f03e1d33c001';
const AUDIO_UUID = '3e7a0002-e33b-4e2f-9a85-f03e1d33c001';
const CONTROL_UUID = '3e7a0003-e33b-4e2f-9a85-f03e1d33c001';
const SAMPLE_RATE = 16000;
const ROMANIZATION_MODEL = 'gpt-4.1-mini';
const HISTORY_MAX = 8;
const HISTORY_STORAGE_KEY = 'transcript_history_v1';
const MOOD_STORAGE_KEY = 'mood_selection_v1';

let DEFAULT_OPENAI_API_KEY = '';
try {
  // mobile-app/secrets.js — gitignored. Example: export const OPENAI_API_KEY = 'sk-...';
  DEFAULT_OPENAI_API_KEY = require('./secrets').OPENAI_API_KEY || '';
} catch (e) {
  DEFAULT_OPENAI_API_KEY = '';
}

// ---------------------------------------------------------------------------
// THEME
// ---------------------------------------------------------------------------

const C = {
  bg: '#0B0D0C',
  surface: '#13171A',
  surfaceAlt: '#181D20',
  surfaceHi: '#1E2428',
  hairline: '#2A2F2D',
  hairlineSoft: '#1F2422',
  fg: '#ECEAE3',
  fgDim: '#B6B4AB',
  muted: '#6F7670',
  mutedDim: '#4A4F4B',
  ok: '#52F2A9',
  danger: '#FF5538',
  warn: '#FFC857',
};

const FONT = {
  display: 'MajorMonoDisplay_400Regular',
  mono: 'JetBrainsMono_400Regular',
  monoMd: 'JetBrainsMono_500Medium',
  monoBd: 'JetBrainsMono_700Bold',
};

// ---------------------------------------------------------------------------
// MOOD CATALOG — must match firmware mood names + colors
// ---------------------------------------------------------------------------

const MOODS = [
  { id: 'PULSE',  label: 'pulse',  vibe: 'alive',     primary: '#FF8030', shade: '#5A2F0F' },
  { id: 'BLOOM',  label: 'bloom',  vibe: 'in love',   primary: '#FF50A0', shade: '#5A1F45' },
  { id: 'DRIFT',  label: 'drift',  vibe: 'calm',      primary: '#50A0FF', shade: '#1F3D5A' },
  { id: 'STATIC', label: 'static', vibe: 'anxious',   primary: '#E8E8E8', shade: '#3F3F3F' },
  { id: 'STORM',  label: 'storm',  vibe: 'angry',     primary: '#FF3030', shade: '#5A1212' },
  { id: 'ORBIT',  label: 'orbit',  vibe: 'curious',   primary: '#A080FF', shade: '#322857' },
  { id: 'GRID',   label: 'grid',   vibe: 'focused',   primary: '#B0FF45', shade: '#3D5A1A' },
  { id: 'PRISM',  label: 'prism',  vibe: 'party',     primary: '#FFD040', shade: '#5A4710' },
];

const moodById = (id) => MOODS.find((m) => m.id === id) || MOODS[6];

// Lightweight transcript-to-mood suggestion based on keyword cues.
// Returns a mood id, or null if nothing strong matched.
function suggestMoodFromText(text) {
  if (!text) return null;
  const t = text.toLowerCase();
  const has = (re) => re.test(t);

  if (has(/\b(love|loving|kiss|heart|adore|crush|miss you|sweetheart|babe|honey)\b/)) return 'BLOOM';
  if (has(/\b(angry|mad|hate|furious|annoy|wtf|damn|stupid|hell)\b/)) return 'STORM';
  if (has(/\b(sad|cry|lonely|sorry|tired|exhaust|hurt|miss|gone|broken)\b/)) return 'DRIFT';
  if (has(/\b(happy|yay|great|awesome|amazing|nice|good|cool|haha|lol|fun)\b/)) return 'PULSE';
  if (has(/\b(why|what|how|when|where|who|wonder|curious|maybe|hmm|think)\b/)) return 'ORBIT';
  if (has(/\b(party|dance|drink|celebrate|birthday|cheers|let'?s go|woo)\b/)) return 'PRISM';
  if (has(/\b(scared|worried|nervous|anxious|panic|stress|weird|creepy)\b/)) return 'STATIC';
  if (has(/\b(focus|work|task|todo|plan|build|fix|done|ship)\b/)) return 'GRID';
  return null;
}

// ---------------------------------------------------------------------------
// SESSION STATE
// ---------------------------------------------------------------------------

const PHASE = {
  Idle: 'idle',
  Scanning: 'scanning',
  Connecting: 'connecting',
  Linked: 'linked',
  Recording: 'recording',
  Uploading: 'uploading',
};

const initialSession = {
  phase: PHASE.Idle,
  status: 'standby · tap to link',
  transcript: '',
  duration: 0,
  history: [],
  fault: null,
  mood: 'GRID',
};

function sessionReducer(state, action) {
  switch (action.type) {
    case 'SCAN_START':
      return {
        ...state,
        phase: PHASE.Scanning,
        status: `scanning · ${DEVICE_NAME.toLowerCase()}`,
        fault: null,
        transcript: '',
        duration: 0,
      };
    case 'SCAN_FAIL':
      return { ...state, phase: PHASE.Idle, status: action.message };
    case 'CONNECTING':
      return { ...state, phase: PHASE.Connecting, status: 'handshake' };
    case 'LINKED':
      return { ...state, phase: PHASE.Linked, status: 'link established' };
    case 'DISCONNECT':
      return {
        ...state,
        phase: PHASE.Idle,
        status: action.reason || 'link severed',
      };
    case 'REC_START':
      return {
        ...state,
        phase: PHASE.Recording,
        status: 'capturing',
        transcript: '',
        duration: 0,
        fault: null,
      };
    case 'REC_STOP':
      return {
        ...state,
        phase: PHASE.Uploading,
        status: 'transcribing',
      };
    case 'TRANSCRIPT_DONE': {
      const entry = {
        id: action.id,
        text: action.text,
        durationSec: action.duration,
        at: Date.now(),
        mood: action.suggestedMood || state.mood,
      };
      return {
        ...state,
        phase: PHASE.Linked,
        status: action.suggestedMood
          ? `mood · ${action.suggestedMood.toLowerCase()}`
          : 'transcript dispatched',
        transcript: action.text,
        duration: action.duration,
        history: [entry, ...state.history].slice(0, HISTORY_MAX),
        mood: action.suggestedMood || state.mood,
      };
    }
    case 'FAULT':
      return {
        ...state,
        phase:
          state.phase === PHASE.Recording || state.phase === PHASE.Uploading
            ? PHASE.Linked
            : state.phase,
        status: action.message,
        fault: action.message,
      };
    case 'STATUS':
      return { ...state, status: action.status };
    case 'SET_MOOD':
      return { ...state, mood: action.mood };
    case 'HYDRATE_HISTORY':
      return { ...state, history: action.history };
    case 'HYDRATE_MOOD':
      return { ...state, mood: action.mood };
    case 'CLEAR_HISTORY':
      return { ...state, history: [] };
    default:
      return state;
  }
}

// ---------------------------------------------------------------------------
// APP
// ---------------------------------------------------------------------------

export default function App() {
  const insets = useSafeAreaInsets();
  const manager = useMemo(() => new BleManager(), []);
  const [recordingMs, setRecordingMs] = useState(0);
  const [, setTick] = useState(0);
  const [session, dispatch] = useReducer(sessionReducer, initialSession);

  const audioChunksRef = useRef([]);
  const controlLineRef = useRef('');
  const subscriptionsRef = useRef([]);
  const deviceRef = useRef(null);
  const isScanningRef = useRef(false);
  const recordingStartRef = useRef(0);
  const recordingTimerRef = useRef(null);
  const tickIntervalRef = useRef(null);
  const sessionRef = useRef(session);

  useEffect(() => {
    sessionRef.current = session;
  }, [session]);

  const [fontsLoaded] = useMajorMono({
    MajorMonoDisplay_400Regular,
    JetBrainsMono_400Regular,
    JetBrainsMono_500Medium,
    JetBrainsMono_700Bold,
  });

  const heroPulse = useRef(new Animated.Value(0)).current;
  const introOpacity = useRef(new Animated.Value(0)).current;
  const introY = useRef(new Animated.Value(12)).current;
  const recordingPulse = useRef(new Animated.Value(0)).current;

  const { phase, status, transcript, duration, history, fault, mood: moodId } = session;
  const mood = moodById(moodId);
  const isConnected = phase === PHASE.Linked || phase === PHASE.Recording || phase === PHASE.Uploading;
  const isScanning = phase === PHASE.Scanning || phase === PHASE.Connecting;
  const isRecording = phase === PHASE.Recording;
  const isTranscribing = phase === PHASE.Uploading;

  // Persisted state hydration
  useEffect(() => {
    SecureStore.getItemAsync(HISTORY_STORAGE_KEY).then((raw) => {
      if (!raw) return;
      try {
        const parsed = JSON.parse(raw);
        if (Array.isArray(parsed)) {
          dispatch({ type: 'HYDRATE_HISTORY', history: parsed.slice(0, HISTORY_MAX) });
        }
      } catch (e) {}
    });
    SecureStore.getItemAsync(MOOD_STORAGE_KEY).then((stored) => {
      if (stored && MOODS.some((m) => m.id === stored)) {
        dispatch({ type: 'HYDRATE_MOOD', mood: stored });
      }
    });

    return () => {
      stopScan();
      subscriptionsRef.current.forEach((subscription) => subscription.remove());
      manager.destroy();
    };
  }, [manager]);

  // Persist
  useEffect(() => {
    SecureStore.setItemAsync(HISTORY_STORAGE_KEY, JSON.stringify(history)).catch(() => {});
  }, [history]);
  useEffect(() => {
    SecureStore.setItemAsync(MOOD_STORAGE_KEY, moodId).catch(() => {});
  }, [moodId]);

  // Intro fade-in
  useEffect(() => {
    if (!fontsLoaded) return;
    Animated.parallel([
      Animated.timing(introOpacity, {
        toValue: 1, duration: 540, easing: Easing.out(Easing.cubic), useNativeDriver: true,
      }),
      Animated.timing(introY, {
        toValue: 0, duration: 540, easing: Easing.out(Easing.cubic), useNativeDriver: true,
      }),
    ]).start();
  }, [fontsLoaded, introOpacity, introY]);

  // Hero ambient pulse — always running, slower when idle, faster when recording
  useEffect(() => {
    const period = isRecording ? 600 : 2200;
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(heroPulse, {
          toValue: 1, duration: period / 2, easing: Easing.inOut(Easing.quad), useNativeDriver: true,
        }),
        Animated.timing(heroPulse, {
          toValue: 0, duration: period / 2, easing: Easing.inOut(Easing.quad), useNativeDriver: true,
        }),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [isRecording, heroPulse]);

  // Recording pulse
  useEffect(() => {
    if (!isRecording) {
      recordingPulse.setValue(0);
      return;
    }
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(recordingPulse, {
          toValue: 1, duration: 700, easing: Easing.inOut(Easing.quad), useNativeDriver: true,
        }),
        Animated.timing(recordingPulse, {
          toValue: 0, duration: 700, easing: Easing.inOut(Easing.quad), useNativeDriver: true,
        }),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [isRecording, recordingPulse]);

  // Recording timer
  useEffect(() => {
    if (isRecording) {
      recordingStartRef.current = Date.now();
      setRecordingMs(0);
      recordingTimerRef.current = setInterval(() => {
        setRecordingMs(Date.now() - recordingStartRef.current);
      }, 80);
    } else if (recordingTimerRef.current) {
      clearInterval(recordingTimerRef.current);
      recordingTimerRef.current = null;
    }
    return () => {
      if (recordingTimerRef.current) {
        clearInterval(recordingTimerRef.current);
        recordingTimerRef.current = null;
      }
    };
  }, [isRecording]);

  // "Xs ago" tick
  useEffect(() => {
    tickIntervalRef.current = setInterval(() => setTick((n) => n + 1), 5000);
    return () => clearInterval(tickIntervalRef.current);
  }, []);

  async function connect() {
    if (!DEFAULT_OPENAI_API_KEY) {
      Alert.alert(
        'Missing key',
        'Set OPENAI_API_KEY in mobile-app/secrets.js and rebuild.',
      );
      return;
    }

    const permitted = await requestBluetoothPermissions();
    if (!permitted) {
      dispatch({ type: 'STATUS', status: 'bluetooth permission denied' });
      return;
    }

    dispatch({ type: 'SCAN_START' });
    isScanningRef.current = true;

    manager.startDeviceScan([SERVICE_UUID], { allowDuplicates: false }, async (error, scannedDevice) => {
      if (error) {
        dispatch({ type: 'SCAN_FAIL', message: error.message });
        stopScan();
        return;
      }
      if (!scannedDevice) return;

      stopScan();
      dispatch({ type: 'CONNECTING' });
      try {
        const connected = await scannedDevice.connect({ requestMTU: 247 });
        const ready = await connected.discoverAllServicesAndCharacteristics();
        deviceRef.current = ready;
        dispatch({ type: 'LINKED' });
        monitorStick(ready);
        await sendControl('STATE:Ready\n', ready);
        // Push current mood to stick on connect.
        await sendControl(`MOOD:${sessionRef.current.mood}\n`, ready);
      } catch (connectError) {
        dispatch({ type: 'SCAN_FAIL', message: connectError.message });
      }
    });

    setTimeout(() => {
      if (isScanningRef.current) {
        stopScan();
        dispatch({ type: 'SCAN_FAIL', message: 'scan timed out' });
      }
    }, 12000);
  }

  function stopScan() {
    manager.stopDeviceScan();
    isScanningRef.current = false;
  }

  async function disconnect() {
    stopScan();
    subscriptionsRef.current.forEach((subscription) => subscription.remove());
    subscriptionsRef.current = [];
    if (deviceRef.current) {
      await deviceRef.current.cancelConnection().catch(() => {});
    }
    deviceRef.current = null;
    dispatch({ type: 'DISCONNECT' });
  }

  function monitorStick(connectedDevice) {
    const controlSub = connectedDevice.monitorCharacteristicForService(
      SERVICE_UUID, CONTROL_UUID,
      (error, characteristic) => {
        if (error) {
          dispatch({ type: 'STATUS', status: error.message });
          return;
        }
        handleControlValue(characteristic?.value);
      },
    );

    const audioSub = connectedDevice.monitorCharacteristicForService(
      SERVICE_UUID, AUDIO_UUID,
      (error, characteristic) => {
        if (error) {
          dispatch({ type: 'STATUS', status: error.message });
          return;
        }
        if (!characteristic?.value) return;
        audioChunksRef.current.push(Buffer.from(characteristic.value, 'base64'));
      },
    );

    subscriptionsRef.current = [controlSub, audioSub];
  }

  function handleControlValue(base64Value) {
    if (!base64Value) return;
    const text = Buffer.from(base64Value, 'base64').toString('utf8');
    for (const ch of text) {
      if (ch === '\n') {
        const line = controlLineRef.current;
        controlLineRef.current = '';
        handleControlLine(line);
      } else {
        controlLineRef.current += ch;
      }
    }
  }

  function handleControlLine(line) {
    if (line === 'START') {
      audioChunksRef.current = [];
      dispatch({ type: 'REC_START' });
      sendControl('STATE:Recording\n');
      return;
    }
    if (line === 'STOP') {
      const chunks = audioChunksRef.current.slice();
      audioChunksRef.current = [];
      const elapsedMs = Date.now() - recordingStartRef.current;
      dispatch({ type: 'REC_STOP' });
      transcribeChunks(chunks, elapsedMs);
      return;
    }
    if (line.startsWith('MOOD:')) {
      const id = line.slice(5).trim().toUpperCase();
      if (MOODS.some((m) => m.id === id)) {
        dispatch({ type: 'SET_MOOD', mood: id });
      }
    }
  }

  // User tapped a mood tile in the app.
  async function pickMood(id) {
    if (id === sessionRef.current.mood) return;
    dispatch({ type: 'SET_MOOD', mood: id });
    await sendControl(`MOOD:${id}\n`);
  }

  async function transcribeChunks(chunks, durationMs) {
    if (!chunks.length) {
      dispatch({ type: 'FAULT', message: 'no audio received' });
      await sendControl('ERR:No audio\n');
      return;
    }

    let wavFile = null;
    try {
      await sendControl('STATE:Transcribing\n');

      const wav = buildWav(chunks);
      const wavBytes = new Uint8Array(wav.buffer, wav.byteOffset, wav.byteLength);

      wavFile = new File(Paths.cache, `m5-stick-${Date.now()}.wav`);
      if (wavFile.exists) wavFile.delete();
      wavFile.create();
      wavFile.write(wavBytes);

      const form = new FormData();
      form.append('model', 'gpt-4o-transcribe');
      form.append('response_format', 'json');
      form.append(
        'prompt',
        'Transcribe the speech in the original language, but always use Latin/English letters. Do not translate the meaning. If the speaker uses Arabic, Hindi, or another non-Latin script language, romanize/transliterate the words phonetically.',
      );
      form.append('file', { uri: wavFile.uri, name: 'm5-stick.wav', type: 'audio/wav' });

      const response = await fetch('https://api.openai.com/v1/audio/transcriptions', {
        method: 'POST',
        headers: { Authorization: `Bearer ${DEFAULT_OPENAI_API_KEY.trim()}` },
        body: form,
      });

      const payload = await response.json().catch(() => ({}));
      if (!response.ok) {
        const message = payload?.error?.message || `OpenAI error ${response.status}`;
        throw new Error(message);
      }

      const rawText = payload.text?.trim() || '(no speech detected)';
      const text = await ensureLatinScript(rawText, DEFAULT_OPENAI_API_KEY.trim());
      const durationSec = Math.max(0, Math.round(durationMs / 100) / 10);

      const suggestedMood = suggestMoodFromText(text);

      dispatch({
        type: 'TRANSCRIPT_DONE',
        id: Date.now(),
        text,
        duration: durationSec,
        suggestedMood,
      });
      await sendControl(`TEXT:${sanitizeControlText(text)}\n`);
      if (suggestedMood && suggestedMood !== sessionRef.current.mood) {
        await sendControl(`MOOD:${suggestedMood}\n`);
      }
    } catch (error) {
      dispatch({ type: 'FAULT', message: error.message });
      await sendControl(`ERR:${sanitizeControlText(error.message)}\n`);
    } finally {
      if (wavFile) {
        try {
          wavFile.delete();
        } catch {}
      }
    }
  }

  async function sendControl(message, targetDevice = deviceRef.current) {
    if (!targetDevice) {
      console.warn('[ctrl->] no device, drop:', message.trim());
      return;
    }

    const bytes = Buffer.from(message, 'utf8');
    console.log(`[ctrl->] ${bytes.length}B: ${message.trim()}`);
    try {
      for (let offset = 0; offset < bytes.length; offset += 160) {
        const chunk = bytes.subarray(offset, offset + 160);
        await targetDevice.writeCharacteristicWithoutResponseForService(
          SERVICE_UUID, CONTROL_UUID, bytesToBase64(chunk),
        );
      }
    } catch (writeError) {
      console.warn('[ctrl->] write failed:', writeError?.message || writeError);
      dispatch({ type: 'STATUS', status: `ble write failed: ${writeError?.message || writeError}` });
    }
  }

  if (!fontsLoaded) {
    return (
      <SafeAreaView style={styles.screen} edges={['top', 'left', 'right', 'bottom']}>
        <StatusBar style="light" backgroundColor={C.bg} />
      </SafeAreaView>
    );
  }

  const linkBadge = (() => {
    if (isRecording) return { dot: C.danger, label: 'rec' };
    if (isTranscribing) return { dot: C.warn, label: 'uplnk' };
    if (phase === PHASE.Linked) return { dot: mood.primary, label: 'linked' };
    if (isScanning) return { dot: C.warn, label: phase === PHASE.Scanning ? 'scan' : 'shake' };
    return { dot: C.muted, label: 'offline' };
  })();

  const heroOpacity = heroPulse.interpolate({ inputRange: [0, 1], outputRange: [0.65, 1] });
  const heroScale = heroPulse.interpolate({ inputRange: [0, 1], outputRange: [1, 1.06] });
  const recordingScale = recordingPulse.interpolate({ inputRange: [0, 1], outputRange: [1, 1.55] });
  const recordingOpacity = recordingPulse.interpolate({ inputRange: [0, 1], outputRange: [0.85, 0] });

  const transcriptText = transcript || 'await · transmission';
  const transcriptIsPlaceholder = !transcript;

  return (
    <SafeAreaView
      style={[styles.screen, { backgroundColor: C.bg }]}
      edges={['top', 'left', 'right']}
    >
      <StatusBar style="light" backgroundColor={C.bg} />
      <Atmosphere accent={mood.primary} />

      <Animated.View
        style={[styles.body, { opacity: introOpacity, transform: [{ translateY: introY }] }]}
      >
        <ScrollView
          contentContainerStyle={[
            styles.scroll,
            { paddingBottom: 36 + Math.max(insets.bottom, 12) },
          ]}
          showsVerticalScrollIndicator={false}
        >
          {/* TOP STRIP */}
          <View style={styles.topBar}>
            <View style={styles.topBarLeft}>
              <Text style={[styles.topMark, { color: mood.primary }]}>m5vs</Text>
              <Text style={styles.topMarkDim}>· necklace</Text>
            </View>
            <View style={styles.topBarRight}>
              <Text style={styles.topMarkDim}>{linkBadge.label}</Text>
              <View style={[styles.linkDot, { backgroundColor: linkBadge.dot }]} />
            </View>
          </View>

          {/* HERO — current mood preview */}
          <View style={styles.hero}>
            <Text style={styles.heroEyebrow}>broadcasting</Text>

            <Animated.View
              style={[
                styles.heroFrame,
                {
                  borderColor: mood.primary,
                  shadowColor: mood.primary,
                  opacity: heroOpacity,
                  transform: [{ scale: heroScale }],
                },
              ]}
            >
              <View style={[styles.heroFill, { backgroundColor: mood.shade }]} />
              <MoodGlyph mood={mood} size={140} />
              <View style={styles.heroCornerTL} />
              <View style={styles.heroCornerTR} />
              <View style={styles.heroCornerBL} />
              <View style={styles.heroCornerBR} />
            </Animated.View>

            <Text style={[styles.heroLabel, { color: mood.primary }]}>{mood.label}</Text>
            <Text style={styles.heroVibe}>· {mood.vibe} ·</Text>
          </View>

          {/* MOOD GRID */}
          <View style={styles.moodGrid}>
            {MOODS.map((m) => (
              <MoodTile
                key={m.id}
                mood={m}
                active={m.id === moodId}
                onPress={() => pickMood(m.id)}
              />
            ))}
          </View>

          {/* DIVIDER */}
          <View style={styles.divider}>
            <View style={styles.dividerLine} />
            <Text style={styles.dividerLabel}>voice</Text>
            <View style={styles.dividerLine} />
          </View>

          {/* CONNECT + RECORDING STRIP */}
          <View style={styles.commsCard}>
            <Pressable
              disabled={isScanning || isTranscribing}
              onPress={isConnected ? disconnect : connect}
              style={({ pressed }) => [
                styles.commsAction,
                {
                  backgroundColor: isConnected ? C.surfaceAlt : mood.primary,
                  borderColor: isConnected ? mood.primary : 'transparent',
                },
                pressed && { opacity: 0.85 },
                (isScanning || isTranscribing) && {
                  backgroundColor: C.surfaceAlt,
                  borderColor: C.hairline,
                },
              ]}
            >
              <Text
                style={[
                  styles.commsActionLabel,
                  { color: isConnected ? mood.primary : C.bg },
                  (isScanning || isTranscribing) && { color: C.fgDim },
                ]}
              >
                {isTranscribing ? 'uploading' :
                 isScanning ? 'searching' :
                 isConnected ? 'sever' : 'connect'}
              </Text>
              {!isConnected && !isScanning && !isTranscribing && (
                <Text style={[styles.commsActionGlyph, { color: C.bg }]}>↗</Text>
              )}
            </Pressable>

            <View style={styles.commsRow}>
              <View style={styles.recBox}>
                {isRecording ? (
                  <View style={styles.recDotWrap}>
                    <Animated.View
                      style={[
                        styles.recDotPulse,
                        { opacity: recordingOpacity, transform: [{ scale: recordingScale }] },
                      ]}
                    />
                    <View style={styles.recDot} />
                  </View>
                ) : (
                  <View style={[styles.recDot, { backgroundColor: isConnected ? mood.primary : C.hairline }]} />
                )}
                <Text style={[
                  styles.recLabel,
                  isRecording && { color: C.danger },
                ]}>
                  {isRecording ? 'rec' : isTranscribing ? 'up' : isConnected ? 'rdy' : 'off'}
                </Text>
              </View>

              <Text style={styles.timer}>{formatDuration(recordingMs)}</Text>

              <View style={styles.signal}>
                {[...Array(7)].map((_, i) => {
                  const active = isRecording
                    ? Math.random() > i / 8
                    : isConnected
                    ? i < 3
                    : false;
                  return (
                    <View
                      key={i}
                      style={[
                        styles.signalBar,
                        { backgroundColor: active ? mood.primary : C.hairline },
                      ]}
                    />
                  );
                })}
              </View>
            </View>

            <View style={styles.statusLine}>
              <View style={[styles.statusDot, { backgroundColor: fault ? C.danger : linkBadge.dot }]} />
              <Text style={styles.statusText} numberOfLines={2}>
                {fault || status}
              </Text>
            </View>
          </View>

          {/* TRANSCRIPT */}
          <View style={styles.transcript}>
            <View style={styles.transcriptHead}>
              <Text style={[styles.transcriptLabel, { color: mood.primary }]}>transcript</Text>
              <Text style={styles.transcriptMeta}>
                {transcript ? `${duration.toFixed(1)}s · ok` : '— · —'}
              </Text>
            </View>

            <Corners color={mood.primary} />

            <ScrollView
              style={styles.transcriptBody}
              contentContainerStyle={styles.transcriptBodyContent}
            >
              <Text
                style={[
                  styles.transcriptText,
                  transcriptIsPlaceholder && styles.transcriptPlaceholder,
                ]}
                selectable
              >
                {transcriptText}
              </Text>
            </ScrollView>

            <View style={styles.transcriptFoot}>
              <Text style={styles.transcriptFootText}>
                gpt-4o-transcribe · latin · {DEVICE_NAME.toLowerCase()}
              </Text>
            </View>
          </View>

          {/* ARCHIVE */}
          <View style={styles.archive}>
            <View style={styles.archiveHead}>
              <Text style={styles.archiveLabel}>archive</Text>
              {history.length > 0 ? (
                <Pressable hitSlop={8} onPress={() => dispatch({ type: 'CLEAR_HISTORY' })}>
                  <Text style={styles.archiveClear}>clear ✕</Text>
                </Pressable>
              ) : (
                <Text style={styles.archiveMeta}>{history.length}/{HISTORY_MAX}</Text>
              )}
            </View>

            {history.length === 0 ? (
              <Text style={styles.archiveEmpty}>
                no transmissions · hold the m5 button to capture
              </Text>
            ) : (
              <View style={styles.archiveList}>
                {history.map((entry, index) => {
                  const entryMood = entry.mood ? moodById(entry.mood) : null;
                  return (
                    <View key={entry.id} style={styles.archiveRow}>
                      <View style={styles.archiveRowMeta}>
                        <Text style={[styles.archiveIndex, entryMood && { color: entryMood.primary }]}>
                          {String(index + 1).padStart(2, '0')}
                        </Text>
                        <Text style={styles.archiveAge}>{relativeTime(entry.at)}</Text>
                        <Text style={styles.archiveDuration}>
                          {entry.durationSec.toFixed(1)}s
                        </Text>
                        {entryMood && (
                          <Text style={[styles.archiveMood, { color: entryMood.primary }]}>
                            · {entryMood.label}
                          </Text>
                        )}
                      </View>
                      <Text style={styles.archiveText} numberOfLines={3} selectable>
                        {entry.text}
                      </Text>
                    </View>
                  );
                })}
              </View>
            )}
          </View>

          {/* HINT */}
          <View style={styles.hints}>
            <HintRow glyph="A" label="hold · talk     dbl · hands-free" />
            <HintRow glyph="B" label="tap · cycle mood     long · info" />
          </View>
        </ScrollView>
      </Animated.View>
    </SafeAreaView>
  );
}

// ---------------------------------------------------------------------------
// MOOD GLYPH — a unique abstract signature per mood, built from plain Views.
// Used both as the big hero preview and the small tile icons.
// ---------------------------------------------------------------------------

function MoodGlyph({ mood, size = 80, dim = false }) {
  const fade = dim ? 0.55 : 1;
  const inner = size * 0.7;

  switch (mood.id) {
    case 'PULSE':
      return (
        <View style={[gs.wrap, { width: size, height: size, opacity: fade }]}>
          {[0.95, 0.7, 0.45, 0.22].map((s, i) => (
            <View
              key={i}
              style={[
                gs.absRing,
                {
                  width: size * s,
                  height: size * s,
                  borderRadius: (size * s) / 2,
                  borderColor: mood.primary,
                  borderWidth: i === 3 ? 0 : 1,
                  backgroundColor: i === 3 ? mood.primary : 'transparent',
                  opacity: 0.3 + (1 - s) * 0.7,
                },
              ]}
            />
          ))}
        </View>
      );

    case 'BLOOM':
      return (
        <View style={[gs.wrap, { width: size, height: size, opacity: fade }]}>
          {[0, 60, 120, 180, 240, 300].map((deg) => (
            <View
              key={deg}
              style={[
                gs.absDot,
                {
                  transform: [
                    { rotate: `${deg}deg` },
                    { translateY: -inner / 2.5 },
                  ],
                  width: size / 5.5,
                  height: size / 5.5,
                  borderRadius: size / 11,
                  backgroundColor: mood.primary,
                },
              ]}
            />
          ))}
          <View style={{
            width: size / 4,
            height: size / 4,
            borderRadius: size / 8,
            backgroundColor: mood.primary,
          }} />
        </View>
      );

    case 'DRIFT':
      return (
        <View style={[gs.wrapColumn, { width: size, height: size, opacity: fade }]}>
          {[0, 1, 2].map((i) => (
            <View
              key={i}
              style={{
                width: size * 0.85,
                height: 2,
                backgroundColor: mood.primary,
                marginVertical: size * 0.08,
                opacity: i === 1 ? 1 : 0.5,
                borderRadius: 1,
                transform: [
                  { rotate: i === 0 ? '-3deg' : i === 1 ? '0deg' : '4deg' },
                  { translateX: i === 0 ? -4 : i === 2 ? 4 : 0 },
                ],
              }}
            />
          ))}
        </View>
      );

    case 'STATIC': {
      const cells = 25;
      return (
        <View style={[gs.staticWrap, { width: size, height: size, opacity: fade }]}>
          {Array.from({ length: cells }).map((_, i) => {
            const on = (i * 7 + 3) % 5 < 2;
            return (
              <View
                key={i}
                style={{
                  width: size / 5 - 2,
                  height: size / 5 - 2,
                  margin: 1,
                  backgroundColor: on ? mood.primary : 'transparent',
                  opacity: on ? 0.7 + ((i % 3) * 0.1) : 0,
                }}
              />
            );
          })}
        </View>
      );
    }

    case 'STORM':
      return (
        <View style={[gs.wrap, { width: size, height: size, opacity: fade }]}>
          <View style={{
            width: 3, height: size * 0.32, backgroundColor: mood.primary,
            transform: [{ rotate: '15deg' }, { translateY: -size * 0.18 }, { translateX: -size * 0.05 }],
          }} />
          <View style={{
            width: 3, height: size * 0.28, backgroundColor: mood.primary,
            transform: [{ rotate: '-22deg' }, { translateY: 0 }, { translateX: size * 0.03 }],
          }} />
          <View style={{
            width: 3, height: size * 0.32, backgroundColor: mood.primary,
            transform: [{ rotate: '12deg' }, { translateY: size * 0.18 }, { translateX: size * 0.06 }],
          }} />
          <View style={[gs.absDot, {
            width: size * 0.12, height: size * 0.12, borderRadius: size * 0.06,
            backgroundColor: mood.primary, top: size * 0.04, left: size * 0.04,
            opacity: 0.5,
          }]} />
        </View>
      );

    case 'ORBIT': {
      return (
        <View style={[gs.wrap, { width: size, height: size, opacity: fade }]}>
          {[0.95, 0.65, 0.35].map((s, i) => (
            <View
              key={i}
              style={[
                gs.absRing,
                {
                  width: size * s, height: size * s, borderRadius: (size * s) / 2,
                  borderColor: mood.primary, borderWidth: 1, opacity: 0.35,
                },
              ]}
            />
          ))}
          {[0, 120, 240].map((deg, i) => (
            <View
              key={deg}
              style={[
                gs.absDot,
                {
                  width: size * 0.12, height: size * 0.12, borderRadius: size * 0.06,
                  backgroundColor: mood.primary,
                  transform: [{ rotate: `${deg}deg` }, { translateY: -size * (0.45 - i * 0.15) }],
                },
              ]}
            />
          ))}
          <View style={{
            width: size * 0.16, height: size * 0.16, borderRadius: size * 0.08,
            backgroundColor: mood.primary,
          }} />
        </View>
      );
    }

    case 'GRID': {
      return (
        <View style={[gs.gridWrap, { width: size, height: size, opacity: fade }]}>
          {Array.from({ length: 9 }).map((_, i) => {
            const big = i === 4;
            const med = [1, 3, 5, 7].includes(i);
            return (
              <View key={i} style={[gs.gridCell, { width: size / 3, height: size / 3 }]}>
                <View
                  style={{
                    width: big ? size / 4 : med ? size / 6 : size / 9,
                    height: big ? size / 4 : med ? size / 6 : size / 9,
                    backgroundColor: mood.primary,
                    opacity: big ? 1 : med ? 0.7 : 0.4,
                  }}
                />
              </View>
            );
          })}
        </View>
      );
    }

    case 'PRISM':
      return (
        <View style={[gs.wrap, { width: size, height: size, opacity: fade }]}>
          {[0, 60, 120].map((deg, i) => (
            <View
              key={deg}
              style={{
                position: 'absolute',
                width: size * 0.55,
                height: size * 0.55,
                borderColor: i === 0 ? mood.primary : i === 1 ? mood.shade : '#FF8030',
                borderWidth: 2,
                transform: [{ rotate: `${deg}deg` }],
                opacity: 0.85,
              }}
            />
          ))}
          <View style={{
            width: size * 0.12, height: size * 0.12, borderRadius: size * 0.06,
            backgroundColor: mood.primary,
          }} />
        </View>
      );

    default:
      return <View style={{ width: size, height: size }} />;
  }
}

// ---------------------------------------------------------------------------
// COMPONENTS
// ---------------------------------------------------------------------------

function MoodTile({ mood, active, onPress }) {
  return (
    <Pressable
      onPress={onPress}
      style={({ pressed }) => [
        styles.tile,
        active && { borderColor: mood.primary, backgroundColor: mood.shade + '40' },
        pressed && { opacity: 0.85, transform: [{ scale: 0.98 }] },
      ]}
    >
      <View style={styles.tileIconWrap}>
        <MoodGlyph mood={mood} size={42} dim={!active} />
      </View>
      <Text style={[
        styles.tileLabel,
        active && { color: mood.primary, fontFamily: FONT.monoBd },
      ]}>
        {mood.label}
      </Text>
      {active && <View style={[styles.tileBadge, { backgroundColor: mood.primary }]} />}
    </Pressable>
  );
}

function Corners({ color }) {
  return (
    <>
      <View style={[styles.corner, styles.cornerTL, { borderColor: color }]} />
      <View style={[styles.corner, styles.cornerTR, { borderColor: color }]} />
      <View style={[styles.corner, styles.cornerBL, { borderColor: color }]} />
      <View style={[styles.corner, styles.cornerBR, { borderColor: color }]} />
    </>
  );
}

function Atmosphere({ accent }) {
  return (
    <View pointerEvents="none" style={StyleSheet.absoluteFill}>
      <View style={[styles.gradTop, { backgroundColor: accent, opacity: 0.05 }]} />
      <View style={styles.gradBottom} />
    </View>
  );
}

function HintRow({ glyph, label }) {
  return (
    <View style={styles.hintRow}>
      <View style={styles.hintKey}>
        <Text style={styles.hintKeyText}>{glyph}</Text>
      </View>
      <Text style={styles.hintLabel}>{label}</Text>
    </View>
  );
}

// ---------------------------------------------------------------------------
// HELPERS
// ---------------------------------------------------------------------------

async function requestBluetoothPermissions() {
  if (Platform.OS !== 'android') return true;
  const permissions =
    Platform.Version >= 31
      ? [PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN, PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT]
      : [PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION];
  const result = await PermissionsAndroid.requestMultiple(permissions);
  return permissions.every((permission) => result[permission] === PermissionsAndroid.RESULTS.GRANTED);
}

function buildWav(chunks) {
  const pcmBytes = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  const wav = Buffer.alloc(44 + pcmBytes);
  let offset = 0;

  wav.write('RIFF', offset); offset += 4;
  wav.writeUInt32LE(36 + pcmBytes, offset); offset += 4;
  wav.write('WAVE', offset); offset += 4;
  wav.write('fmt ', offset); offset += 4;
  wav.writeUInt32LE(16, offset); offset += 4;
  wav.writeUInt16LE(1, offset); offset += 2;
  wav.writeUInt16LE(1, offset); offset += 2;
  wav.writeUInt32LE(SAMPLE_RATE, offset); offset += 4;
  wav.writeUInt32LE(SAMPLE_RATE * 2, offset); offset += 4;
  wav.writeUInt16LE(2, offset); offset += 2;
  wav.writeUInt16LE(16, offset); offset += 2;
  wav.write('data', offset); offset += 4;
  wav.writeUInt32LE(pcmBytes, offset); offset += 4;

  for (const chunk of chunks) {
    chunk.copy(wav, offset);
    offset += chunk.length;
  }
  return wav;
}

function sanitizeControlText(text) {
  return String(text).replace(/[\r\n]+/g, ' ').slice(0, 420);
}

function formatDuration(ms) {
  const totalTenths = Math.max(0, Math.round(ms / 100));
  const seconds = Math.floor(totalTenths / 10);
  const tenths = totalTenths % 10;
  const mm = String(Math.floor(seconds / 60)).padStart(2, '0');
  const ss = String(seconds % 60).padStart(2, '0');
  return `${mm}:${ss}.${tenths}`;
}

function relativeTime(timestamp) {
  const diffMs = Date.now() - timestamp;
  const seconds = Math.floor(diffMs / 1000);
  if (seconds < 5) return 'now';
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return `${hours}h`;
  const days = Math.floor(hours / 24);
  return `${days}d`;
}

async function ensureLatinScript(text, apiKey) {
  if (!containsNonLatinScript(text) || !apiKey) return text;

  const response = await fetch('https://api.openai.com/v1/chat/completions', {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${apiKey}`,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({
      model: ROMANIZATION_MODEL,
      temperature: 0,
      messages: [
        {
          role: 'system',
          content:
            'Convert text to Latin/English letters only. Do not translate the meaning. Preserve the original language phonetically using common romanization. Return only the converted text.',
        },
        { role: 'user', content: text },
      ],
    }),
  });

  const payload = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(payload?.error?.message || `Romanization error ${response.status}`);
  }
  return payload?.choices?.[0]?.message?.content?.trim() || text;
}

function containsNonLatinScript(text) {
  return /[^\u0000-\u024F]/.test(text);
}

function bytesToBase64(bytes) {
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  let output = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const a = bytes[i];
    const b = i + 1 < bytes.length ? bytes[i + 1] : 0;
    const c = i + 2 < bytes.length ? bytes[i + 2] : 0;
    const triplet = (a << 16) | (b << 8) | c;
    output += alphabet[(triplet >> 18) & 63];
    output += alphabet[(triplet >> 12) & 63];
    output += i + 1 < bytes.length ? alphabet[(triplet >> 6) & 63] : '=';
    output += i + 2 < bytes.length ? alphabet[triplet & 63] : '=';
  }
  return output;
}

// ---------------------------------------------------------------------------
// STYLES
// ---------------------------------------------------------------------------

const gs = StyleSheet.create({
  wrap: {
    alignItems: 'center',
    justifyContent: 'center',
    position: 'relative',
  },
  wrapColumn: {
    alignItems: 'center',
    justifyContent: 'center',
    flexDirection: 'column',
  },
  absRing: {
    position: 'absolute',
  },
  absDot: {
    position: 'absolute',
  },
  staticWrap: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    alignItems: 'center',
    justifyContent: 'center',
  },
  gridWrap: {
    flexDirection: 'row',
    flexWrap: 'wrap',
  },
  gridCell: {
    alignItems: 'center',
    justifyContent: 'center',
  },
});

const styles = StyleSheet.create({
  screen: { flex: 1 },
  body: { flex: 1 },
  scroll: {
    paddingHorizontal: 22,
    paddingTop: 18,
    // bottom inset applied inline via useSafeAreaInsets + paddingBottom
  },

  gradTop: {
    position: 'absolute',
    top: 0, left: 0, right: 0,
    height: 220,
  },
  gradBottom: {
    position: 'absolute',
    bottom: 0, left: 0, right: 0,
    height: 220,
    backgroundColor: '#06080A',
    opacity: 0.6,
  },

  topBar: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingBottom: 14,
    marginBottom: 10,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: C.hairline,
  },
  topBarLeft: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
  },
  topBarRight: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
  },
  topMark: {
    fontFamily: FONT.monoBd,
    fontSize: 11,
    letterSpacing: 3,
    textTransform: 'lowercase',
  },
  topMarkDim: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.6,
  },
  linkDot: {
    width: 8,
    height: 8,
    borderRadius: 1,
    transform: [{ rotate: '45deg' }],
  },

  hero: {
    alignItems: 'center',
    paddingTop: 18,
    paddingBottom: 6,
  },
  heroEyebrow: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 4,
    textTransform: 'lowercase',
    marginBottom: 18,
  },
  heroFrame: {
    position: 'relative',
    width: 200,
    height: 200,
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 1,
    backgroundColor: C.surface,
    shadowOffset: { width: 0, height: 0 },
    shadowOpacity: 0.6,
    shadowRadius: 24,
    marginBottom: 16,
  },
  heroFill: {
    ...StyleSheet.absoluteFillObject,
    opacity: 0.18,
  },
  heroCornerTL: {
    position: 'absolute', top: -6, left: -6,
    width: 14, height: 14,
    borderTopWidth: 2, borderLeftWidth: 2,
    borderColor: C.fg,
  },
  heroCornerTR: {
    position: 'absolute', top: -6, right: -6,
    width: 14, height: 14,
    borderTopWidth: 2, borderRightWidth: 2,
    borderColor: C.fg,
  },
  heroCornerBL: {
    position: 'absolute', bottom: -6, left: -6,
    width: 14, height: 14,
    borderBottomWidth: 2, borderLeftWidth: 2,
    borderColor: C.fg,
  },
  heroCornerBR: {
    position: 'absolute', bottom: -6, right: -6,
    width: 14, height: 14,
    borderBottomWidth: 2, borderRightWidth: 2,
    borderColor: C.fg,
  },
  heroLabel: {
    fontFamily: FONT.display,
    fontSize: 36,
    letterSpacing: -1,
    marginTop: 4,
    textTransform: 'lowercase',
  },
  heroVibe: {
    color: C.fgDim,
    fontFamily: FONT.mono,
    fontSize: 11,
    letterSpacing: 3.5,
    marginTop: 2,
    marginBottom: 8,
  },

  moodGrid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    justifyContent: 'space-between',
    rowGap: 10,
    columnGap: 10,
    marginTop: 10,
    marginBottom: 16,
  },
  tile: {
    width: '23%',
    aspectRatio: 1,
    backgroundColor: C.surface,
    borderColor: C.hairline,
    borderWidth: 1,
    alignItems: 'center',
    justifyContent: 'center',
    paddingVertical: 6,
    position: 'relative',
  },
  tileIconWrap: {
    width: 42,
    height: 42,
    alignItems: 'center',
    justifyContent: 'center',
    marginBottom: 4,
  },
  tileLabel: {
    color: C.fgDim,
    fontFamily: FONT.mono,
    fontSize: 9,
    letterSpacing: 1.6,
    textTransform: 'lowercase',
  },
  tileBadge: {
    position: 'absolute',
    top: 4,
    right: 4,
    width: 4,
    height: 4,
    borderRadius: 2,
  },

  divider: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    marginTop: 6,
    marginBottom: 14,
  },
  dividerLine: {
    flex: 1,
    height: StyleSheet.hairlineWidth,
    backgroundColor: C.hairline,
  },
  dividerLabel: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 4,
  },

  commsCard: {
    backgroundColor: C.surface,
    borderColor: C.hairline,
    borderWidth: 1,
    paddingHorizontal: 14,
    paddingTop: 14,
    paddingBottom: 12,
    marginBottom: 16,
  },
  commsAction: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingVertical: 14,
    paddingHorizontal: 16,
    marginBottom: 14,
    borderWidth: 1,
  },
  commsActionLabel: {
    fontFamily: FONT.monoBd,
    fontSize: 13,
    letterSpacing: 4,
    textTransform: 'lowercase',
  },
  commsActionGlyph: {
    fontFamily: FONT.monoBd,
    fontSize: 16,
  },
  commsRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingBottom: 10,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: C.hairlineSoft,
  },
  recBox: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    width: 64,
  },
  recDotWrap: {
    width: 16,
    height: 16,
    alignItems: 'center',
    justifyContent: 'center',
  },
  recDotPulse: {
    position: 'absolute',
    width: 16,
    height: 16,
    borderRadius: 8,
    backgroundColor: C.danger,
  },
  recDot: {
    width: 9,
    height: 9,
    borderRadius: 5,
    backgroundColor: C.danger,
  },
  recLabel: {
    color: C.fgDim,
    fontFamily: FONT.monoBd,
    fontSize: 11,
    letterSpacing: 2,
    textTransform: 'uppercase',
  },
  timer: {
    flex: 1,
    textAlign: 'center',
    color: C.fg,
    fontFamily: FONT.monoBd,
    fontSize: 20,
    letterSpacing: 1.5,
  },
  signal: {
    flexDirection: 'row',
    alignItems: 'flex-end',
    gap: 2,
    height: 16,
  },
  signalBar: {
    width: 3,
    height: 16,
  },
  statusLine: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    paddingTop: 10,
  },
  statusDot: {
    width: 6,
    height: 6,
    borderRadius: 3,
  },
  statusText: {
    color: C.fgDim,
    fontFamily: FONT.mono,
    fontSize: 11,
    letterSpacing: 0.4,
    flex: 1,
  },

  transcript: {
    position: 'relative',
    backgroundColor: C.surfaceAlt,
    borderColor: C.hairline,
    borderWidth: 1,
    paddingHorizontal: 18,
    paddingVertical: 14,
    minHeight: 180,
    marginBottom: 18,
  },
  transcriptHead: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingBottom: 10,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: C.hairlineSoft,
  },
  transcriptLabel: {
    fontFamily: FONT.monoBd,
    fontSize: 11,
    letterSpacing: 2.4,
    textTransform: 'lowercase',
  },
  transcriptMeta: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.4,
  },
  transcriptBody: {
    flexGrow: 0,
    paddingVertical: 14,
    maxHeight: 240,
  },
  transcriptBodyContent: {
    paddingRight: 4,
  },
  transcriptText: {
    color: C.fg,
    fontFamily: FONT.monoMd,
    fontSize: 20,
    lineHeight: 30,
    letterSpacing: 0.4,
  },
  transcriptPlaceholder: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 14,
    letterSpacing: 4,
    textTransform: 'lowercase',
  },
  transcriptFoot: {
    paddingTop: 10,
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: C.hairlineSoft,
  },
  transcriptFootText: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 9,
    letterSpacing: 1.6,
  },

  archive: {
    backgroundColor: C.surface,
    borderColor: C.hairline,
    borderWidth: 1,
    paddingHorizontal: 14,
    paddingTop: 12,
    paddingBottom: 14,
    marginBottom: 18,
  },
  archiveHead: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingBottom: 10,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: C.hairlineSoft,
    marginBottom: 12,
  },
  archiveLabel: {
    color: C.fgDim,
    fontFamily: FONT.monoMd,
    fontSize: 10,
    letterSpacing: 2.4,
    textTransform: 'lowercase',
  },
  archiveMeta: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.4,
  },
  archiveClear: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.4,
  },
  archiveEmpty: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 12,
    letterSpacing: 1.4,
    paddingVertical: 4,
  },
  archiveList: {
    gap: 12,
  },
  archiveRow: {
    paddingTop: 10,
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: C.hairlineSoft,
  },
  archiveRowMeta: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingBottom: 6,
  },
  archiveIndex: {
    color: C.fgDim,
    fontFamily: FONT.monoBd,
    fontSize: 11,
    letterSpacing: 1.6,
  },
  archiveAge: {
    color: C.fgDim,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.2,
  },
  archiveDuration: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.2,
    flex: 1,
  },
  archiveMood: {
    fontFamily: FONT.monoBd,
    fontSize: 10,
    letterSpacing: 1.4,
    textTransform: 'lowercase',
  },
  archiveText: {
    color: C.fg,
    fontFamily: FONT.monoMd,
    fontSize: 14,
    lineHeight: 20,
    letterSpacing: 0.3,
  },

  corner: {
    position: 'absolute',
    width: 10,
    height: 10,
  },
  cornerTL: {
    top: -1, left: -1,
    borderTopWidth: 2, borderLeftWidth: 2,
  },
  cornerTR: {
    top: -1, right: -1,
    borderTopWidth: 2, borderRightWidth: 2,
  },
  cornerBL: {
    bottom: -1, left: -1,
    borderBottomWidth: 2, borderLeftWidth: 2,
  },
  cornerBR: {
    bottom: -1, right: -1,
    borderBottomWidth: 2, borderRightWidth: 2,
  },

  hints: {
    gap: 8,
    paddingTop: 6,
  },
  hintRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
  },
  hintKey: {
    width: 22,
    height: 22,
    borderWidth: 1,
    borderColor: C.muted,
    alignItems: 'center',
    justifyContent: 'center',
  },
  hintKeyText: {
    color: C.fgDim,
    fontFamily: FONT.monoBd,
    fontSize: 11,
  },
  hintLabel: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.6,
  },
});
