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
  SafeAreaView,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
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
const API_KEY_STORAGE_KEY = 'openai_api_key';
const ROMANIZATION_MODEL = 'gpt-4.1-mini';
const HISTORY_MAX = 8;
const HISTORY_STORAGE_KEY = 'transcript_history_v1';

let DEFAULT_OPENAI_API_KEY = '';
try {
  // Optional, gitignored local secrets file (mobile-app/secrets.js).
  // Example: export const OPENAI_API_KEY = 'sk-...';

  DEFAULT_OPENAI_API_KEY = require('./secrets').OPENAI_API_KEY || '';
} catch (e) {
  DEFAULT_OPENAI_API_KEY = '';
}

const C = {
  bg: '#0E1110',
  surface: '#14181A',
  surfaceAlt: '#191E20',
  hairline: '#2A2F2D',
  hairlineSoft: '#1F2422',
  fg: '#ECEAE3',
  fgDim: '#B6B4AB',
  muted: '#6F7670',
  accent: '#D6FF45',
  accentDim: '#7E9528',
  danger: '#FF5538',
  ok: '#52F2A9',
};

const FONT = {
  display: 'MajorMonoDisplay_400Regular',
  mono: 'JetBrainsMono_400Regular',
  monoMd: 'JetBrainsMono_500Medium',
  monoBd: 'JetBrainsMono_700Bold',
};

// ---------------------------------------------------------------------------
// SESSION STATE — single reducer for the whole link lifecycle
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
  status: 'Idle',
  transcript: '',
  duration: 0,
  history: [],
  fault: null,
};

function sessionReducer(state, action) {
  switch (action.type) {
    case 'SCAN_START':
      return {
        ...state,
        phase: PHASE.Scanning,
        status: `Scanning · ${DEVICE_NAME}`,
        fault: null,
        transcript: '',
        duration: 0,
      };
    case 'SCAN_FAIL':
      return { ...state, phase: PHASE.Idle, status: action.message };
    case 'CONNECTING':
      return { ...state, phase: PHASE.Connecting, status: 'Handshake' };
    case 'LINKED':
      return { ...state, phase: PHASE.Linked, status: 'Link established' };
    case 'DISCONNECT':
      return {
        ...state,
        phase: PHASE.Idle,
        status: action.reason || 'Link severed',
      };
    case 'REC_START':
      return {
        ...state,
        phase: PHASE.Recording,
        status: 'Capturing audio',
        transcript: '',
        duration: 0,
        fault: null,
      };
    case 'REC_STOP':
      return {
        ...state,
        phase: PHASE.Uploading,
        status: 'Uploading to OpenAI',
      };
    case 'TRANSCRIPT_DONE': {
      const entry = {
        id: action.id,
        text: action.text,
        durationSec: action.duration,
        at: Date.now(),
      };
      return {
        ...state,
        phase: PHASE.Linked,
        status: 'Transcript dispatched',
        transcript: action.text,
        duration: action.duration,
        history: [entry, ...state.history].slice(0, HISTORY_MAX),
      };
    }
    case 'FAULT':
      return {
        ...state,
        phase: state.phase === PHASE.Recording || state.phase === PHASE.Uploading ? PHASE.Linked : state.phase,
        status: action.message,
        fault: action.message,
      };
    case 'STATUS':
      return { ...state, status: action.status };
    case 'HYDRATE_HISTORY':
      return { ...state, history: action.history };
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
  const manager = useMemo(() => new BleManager(), []);
  const [apiKey, setApiKey] = useState(DEFAULT_OPENAI_API_KEY);
  const [showApiKey, setShowApiKey] = useState(false);
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

  const [fontsLoaded] = useMajorMono({
    MajorMonoDisplay_400Regular,
    JetBrainsMono_400Regular,
    JetBrainsMono_500Medium,
    JetBrainsMono_700Bold,
  });

  const introOpacity = useRef(new Animated.Value(0)).current;
  const introY = useRef(new Animated.Value(12)).current;
  const recordingPulse = useRef(new Animated.Value(0)).current;
  const scanRotate = useRef(new Animated.Value(0)).current;

  const { phase, status, transcript, duration, history, fault } = session;
  const isConnected = phase === PHASE.Linked || phase === PHASE.Recording || phase === PHASE.Uploading;
  const isScanning = phase === PHASE.Scanning || phase === PHASE.Connecting;
  const isRecording = phase === PHASE.Recording;
  const isTranscribing = phase === PHASE.Uploading;
  const linkDot = isConnected ? C.accent : isScanning ? C.accent : C.muted;

  // Persisted state hydration
  useEffect(() => {
    SecureStore.getItemAsync(API_KEY_STORAGE_KEY).then((stored) => {
      if (stored) setApiKey(stored);
    });
    SecureStore.getItemAsync(HISTORY_STORAGE_KEY).then((raw) => {
      if (!raw) return;
      try {
        const parsed = JSON.parse(raw);
        if (Array.isArray(parsed)) {
          dispatch({ type: 'HYDRATE_HISTORY', history: parsed.slice(0, HISTORY_MAX) });
        }
      } catch (e) {}
    });

    return () => {
      stopScan();
      subscriptionsRef.current.forEach((subscription) => subscription.remove());
      manager.destroy();
    };
  }, [manager]);

  // Persist history when it changes
  useEffect(() => {
    SecureStore.setItemAsync(HISTORY_STORAGE_KEY, JSON.stringify(history)).catch(() => {});
  }, [history]);

  // Intro animation
  useEffect(() => {
    if (!fontsLoaded) return;
    Animated.parallel([
      Animated.timing(introOpacity, {
        toValue: 1,
        duration: 520,
        easing: Easing.out(Easing.cubic),
        useNativeDriver: true,
      }),
      Animated.timing(introY, {
        toValue: 0,
        duration: 520,
        easing: Easing.out(Easing.cubic),
        useNativeDriver: true,
      }),
    ]).start();
  }, [fontsLoaded, introOpacity, introY]);

  // Recording pulse
  useEffect(() => {
    if (!isRecording) {
      recordingPulse.setValue(0);
      return;
    }
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(recordingPulse, {
          toValue: 1,
          duration: 700,
          easing: Easing.inOut(Easing.quad),
          useNativeDriver: true,
        }),
        Animated.timing(recordingPulse, {
          toValue: 0,
          duration: 700,
          easing: Easing.inOut(Easing.quad),
          useNativeDriver: true,
        }),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [isRecording, recordingPulse]);

  // Scan spin
  useEffect(() => {
    if (!isScanning) {
      scanRotate.setValue(0);
      return;
    }
    const loop = Animated.loop(
      Animated.timing(scanRotate, {
        toValue: 1,
        duration: 1200,
        easing: Easing.linear,
        useNativeDriver: true,
      }),
    );
    loop.start();
    return () => loop.stop();
  }, [isScanning, scanRotate]);

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

  // Generic re-render tick for "Xs ago" labels
  useEffect(() => {
    tickIntervalRef.current = setInterval(() => setTick((n) => n + 1), 5000);
    return () => clearInterval(tickIntervalRef.current);
  }, []);

  async function saveApiKey(value) {
    setApiKey(value);
    if (value.trim()) {
      await SecureStore.setItemAsync(API_KEY_STORAGE_KEY, value.trim());
    } else {
      await SecureStore.deleteItemAsync(API_KEY_STORAGE_KEY);
    }
  }

  async function connect() {
    if (!apiKey.trim()) {
      Alert.alert('Auth key required', 'Add an OpenAI key before initializing the link.');
      return;
    }

    const permitted = await requestBluetoothPermissions();
    if (!permitted) {
      dispatch({ type: 'STATUS', status: 'Bluetooth permission denied' });
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

      if (!scannedDevice) {
        return;
      }

      stopScan();
      dispatch({ type: 'CONNECTING' });
      try {
        const connected = await scannedDevice.connect({ requestMTU: 247 });
        const ready = await connected.discoverAllServicesAndCharacteristics();
        deviceRef.current = ready;
        dispatch({ type: 'LINKED' });
        monitorStick(ready);
        await sendControl('STATE:Ready\n', ready);
      } catch (connectError) {
        dispatch({ type: 'SCAN_FAIL', message: connectError.message });
      }
    });

    setTimeout(() => {
      if (isScanningRef.current) {
        stopScan();
        dispatch({ type: 'SCAN_FAIL', message: 'Scan timed out' });
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
      SERVICE_UUID,
      CONTROL_UUID,
      (error, characteristic) => {
        if (error) {
          dispatch({ type: 'STATUS', status: error.message });
          return;
        }
        handleControlValue(characteristic?.value);
      },
    );

    const audioSub = connectedDevice.monitorCharacteristicForService(
      SERVICE_UUID,
      AUDIO_UUID,
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
    }
  }

  async function transcribeChunks(chunks, durationMs) {
    if (!chunks.length) {
      dispatch({ type: 'FAULT', message: 'No audio received' });
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
      form.append('file', {
        uri: wavFile.uri,
        name: 'm5-stick.wav',
        type: 'audio/wav',
      });

      const response = await fetch('https://api.openai.com/v1/audio/transcriptions', {
        method: 'POST',
        headers: {
          Authorization: `Bearer ${apiKey.trim()}`,
        },
        body: form,
      });

      const payload = await response.json().catch(() => ({}));
      if (!response.ok) {
        const message = payload?.error?.message || `OpenAI error ${response.status}`;
        throw new Error(message);
      }

      const rawText = payload.text?.trim() || '(No speech detected)';
      const text = await ensureLatinScript(rawText, apiKey.trim());
      const durationSec = Math.max(0, Math.round(durationMs / 100) / 10);
      dispatch({
        type: 'TRANSCRIPT_DONE',
        id: Date.now(),
        text,
        duration: durationSec,
      });
      await sendControl(`TEXT:${sanitizeControlText(text)}\n`);
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
          SERVICE_UUID,
          CONTROL_UUID,
          bytesToBase64(chunk),
        );
      }
    } catch (writeError) {
      console.warn('[ctrl->] write failed:', writeError?.message || writeError);
      dispatch({ type: 'STATUS', status: `BLE write failed: ${writeError?.message || writeError}` });
    }
  }

  if (!fontsLoaded) {
    return (
      <SafeAreaView style={styles.screen}>
        <StatusBar style="light" backgroundColor={C.bg} />
      </SafeAreaView>
    );
  }

  const linkLabel = (() => {
    switch (phase) {
      case PHASE.Linked:
      case PHASE.Recording:
      case PHASE.Uploading:
        return 'LINK · LIVE';
      case PHASE.Scanning:
        return 'LINK · SEARCH';
      case PHASE.Connecting:
        return 'LINK · NEGOT';
      default:
        return 'LINK · IDLE';
    }
  })();

  const buttonLabel = (() => {
    if (isTranscribing) return 'PROCESSING…';
    if (isScanning) return 'SCANNING…';
    if (isConnected) return 'SEVER LINK';
    return 'INITIALIZE LINK';
  })();

  const recordingScale = recordingPulse.interpolate({
    inputRange: [0, 1],
    outputRange: [1, 1.55],
  });
  const recordingOpacity = recordingPulse.interpolate({
    inputRange: [0, 1],
    outputRange: [0.85, 0],
  });
  const scanSpin = scanRotate.interpolate({
    inputRange: [0, 1],
    outputRange: ['0deg', '360deg'],
  });

  const transcriptText = transcript || 'await · transmission';
  const transcriptIsPlaceholder = !transcript;

  return (
    <SafeAreaView style={styles.screen}>
      <StatusBar style="light" backgroundColor={C.bg} />
      <Grain />

      <Animated.View
        style={[
          styles.body,
          { opacity: introOpacity, transform: [{ translateY: introY }] },
        ]}
      >
        <ScrollView
          contentContainerStyle={styles.scroll}
          showsVerticalScrollIndicator={false}
        >
          {/* TOP BAR */}
          <View style={styles.topBar}>
            <View style={styles.topBarLeft}>
              <Text style={styles.topMark}>M5VS</Text>
              <Text style={styles.topMarkDim}>·  NODE 01</Text>
            </View>

            <View style={styles.topBarRight}>
              <Text style={[styles.topMarkDim, { marginRight: 10 }]}>{linkLabel}</Text>
              <Animated.View
                style={[
                  styles.linkDot,
                  { backgroundColor: linkDot },
                  isScanning && { transform: [{ rotate: scanSpin }] },
                ]}
              />
            </View>
          </View>

          {/* HERO */}
          <View style={styles.hero}>
            <Text style={styles.heroLine1}>field</Text>
            <Text style={styles.heroLine2}>transmitter</Text>
            <View style={styles.heroMetaRow}>
              <Text style={styles.heroMeta}>v0.2 · push-to-talk · ble 4.2</Text>
              <View style={styles.heroBars}>
                {[0, 1, 2, 3, 4].map((i) => (
                  <View
                    key={i}
                    style={[
                      styles.heroBar,
                      {
                        backgroundColor:
                          isConnected && i < 4
                            ? C.accent
                            : isScanning && i < 2
                            ? C.accent
                            : C.hairline,
                        height: 4 + i * 3,
                      },
                    ]}
                  />
                ))}
              </View>
            </View>
          </View>

          {/* AUTH PANEL */}
          <Panel label="auth · key">
            <View style={styles.inputRow}>
              <TextInput
                value={apiKey}
                onChangeText={saveApiKey}
                placeholder="sk-•••"
                placeholderTextColor={C.muted}
                autoCapitalize="none"
                autoCorrect={false}
                secureTextEntry={!showApiKey}
                style={styles.input}
              />
              <Pressable
                onPress={() => setShowApiKey((value) => !value)}
                hitSlop={10}
                style={styles.inputAction}
              >
                <Text style={styles.inputActionText}>{showApiKey ? 'HIDE' : 'SHOW'}</Text>
              </Pressable>
            </View>
          </Panel>

          {/* PRIMARY ACTION */}
          <Pressable
            disabled={isScanning || isTranscribing}
            onPress={isConnected ? disconnect : connect}
            style={({ pressed }) => [
              styles.primary,
              isConnected && styles.primaryActive,
              pressed && styles.primaryPressed,
              (isScanning || isTranscribing) && styles.primaryDisabled,
            ]}
          >
            <View style={styles.primaryEdge} />
            <View style={styles.primaryRow}>
              <Text style={[styles.primaryGlyph, isConnected && styles.primaryGlyphActive]}>
                {isConnected ? '◇' : '◆'}
              </Text>
              <Text style={[styles.primaryLabel, isConnected && styles.primaryLabelActive]}>
                {buttonLabel}
              </Text>
              <Text style={[styles.primaryGlyph, isConnected && styles.primaryGlyphActive]}>
                {isConnected ? '◇' : '◆'}
              </Text>
            </View>
          </Pressable>

          {/* CHANNEL */}
          <Panel label="channel · 01" right={<Text style={styles.panelMeta}>16k · mono · pcm</Text>}>
            <View style={styles.channelRow}>
              <View style={styles.statRecBox}>
                {isRecording ? (
                  <View style={styles.recDotWrap}>
                    <Animated.View
                      style={[
                        styles.recDotPulse,
                        {
                          opacity: recordingOpacity,
                          transform: [{ scale: recordingScale }],
                        },
                      ]}
                    />
                    <View style={styles.recDot} />
                  </View>
                ) : (
                  <View style={[styles.recDot, { backgroundColor: C.hairline }]} />
                )}
                <Text style={[styles.recLabel, isRecording && { color: C.danger }]}>
                  {isRecording ? 'REC' : isTranscribing ? 'UPLNK' : 'STBY'}
                </Text>
              </View>

              <View style={styles.timer}>
                <Text style={styles.timerNum}>{formatDuration(recordingMs)}</Text>
              </View>

              <View style={styles.signal}>
                {[...Array(8)].map((_, i) => {
                  const active = isRecording
                    ? Math.random() > i / 9
                    : isConnected
                    ? i < 3
                    : false;
                  return (
                    <View
                      key={i}
                      style={[
                        styles.signalBar,
                        { backgroundColor: active ? C.accent : C.hairline },
                      ]}
                    />
                  );
                })}
              </View>
            </View>

            <View style={styles.statusLine}>
              <View
                style={[
                  styles.statusDot,
                  { backgroundColor: fault ? C.danger : linkDot },
                ]}
              />
              <Text style={styles.statusText} numberOfLines={2}>
                {fault || status}
              </Text>
            </View>
          </Panel>

          {/* TRANSCRIPT */}
          <View style={styles.transcript}>
            <View style={styles.transcriptHead}>
              <Text style={styles.transcriptLabel}>transcript</Text>
              <Text style={styles.transcriptMeta}>
                {transcript ? `${duration.toFixed(1)}s · ok` : '— · —'}
              </Text>
            </View>

            <Corners />

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
                gpt-4o-transcribe · latin · {DEVICE_NAME}
              </Text>
            </View>
          </View>

          {/* HISTORY */}
          <Panel
            label="archive"
            right={
              history.length > 0 ? (
                <Pressable
                  hitSlop={8}
                  onPress={() => dispatch({ type: 'CLEAR_HISTORY' })}
                >
                  <Text style={styles.panelMeta}>clear ✕</Text>
                </Pressable>
              ) : (
                <Text style={styles.panelMeta}>{history.length} / {HISTORY_MAX}</Text>
              )
            }
          >
            {history.length === 0 ? (
              <Text style={styles.archiveEmpty}>
                no transmissions · hold the m5 button to capture
              </Text>
            ) : (
              <View style={styles.archiveList}>
                {history.map((entry, index) => (
                  <View key={entry.id} style={styles.archiveRow}>
                    <View style={styles.archiveMeta}>
                      <Text style={styles.archiveIndex}>{String(index + 1).padStart(2, '0')}</Text>
                      <Text style={styles.archiveAge}>{relativeTime(entry.at)}</Text>
                      <Text style={styles.archiveDuration}>
                        {entry.durationSec.toFixed(1)}s
                      </Text>
                    </View>
                    <Text style={styles.archiveText} numberOfLines={3} selectable>
                      {entry.text}
                    </Text>
                  </View>
                ))}
              </View>
            )}
          </Panel>

          {/* CONTROLS HINT */}
          <View style={styles.bottomRule} />
          <View style={styles.legend}>
            <LegendRow glyph="A" label="hold · talk     dbl · lock" />
            <LegendRow glyph="B" label="tap · view      hold · clear" />
            <LegendRow glyph="B" label="dbl · brightness" />
          </View>
        </ScrollView>
      </Animated.View>
    </SafeAreaView>
  );
}

// ---------------------------------------------------------------------------
// COMPONENTS
// ---------------------------------------------------------------------------

function Panel({ label, right, children }) {
  return (
    <View style={styles.panel}>
      <View style={styles.panelHead}>
        <Text style={styles.panelLabel}>{label}</Text>
        {right}
      </View>
      <View style={styles.panelBody}>{children}</View>
    </View>
  );
}

function Corners() {
  return (
    <>
      <View style={[styles.corner, styles.cornerTL]} />
      <View style={[styles.corner, styles.cornerTR]} />
      <View style={[styles.corner, styles.cornerBL]} />
      <View style={[styles.corner, styles.cornerBR]} />
    </>
  );
}

function Grain() {
  return (
    <View pointerEvents="none" style={StyleSheet.absoluteFill}>
      <View style={styles.gradTop} />
      <View style={styles.gradBottom} />
    </View>
  );
}

function LegendRow({ glyph, label }) {
  return (
    <View style={styles.legendRow}>
      <View style={styles.legendKey}>
        <Text style={styles.legendKeyText}>{glyph}</Text>
      </View>
      <Text style={styles.legendLabel}>{label}</Text>
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
      ? [
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
        ]
      : [PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION];

  const result = await PermissionsAndroid.requestMultiple(permissions);
  return permissions.every((permission) => result[permission] === PermissionsAndroid.RESULTS.GRANTED);
}

function buildWav(chunks) {
  const pcmBytes = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  const wav = Buffer.alloc(44 + pcmBytes);
  let offset = 0;

  wav.write('RIFF', offset);
  offset += 4;
  wav.writeUInt32LE(36 + pcmBytes, offset);
  offset += 4;
  wav.write('WAVE', offset);
  offset += 4;
  wav.write('fmt ', offset);
  offset += 4;
  wav.writeUInt32LE(16, offset);
  offset += 4;
  wav.writeUInt16LE(1, offset);
  offset += 2;
  wav.writeUInt16LE(1, offset);
  offset += 2;
  wav.writeUInt32LE(SAMPLE_RATE, offset);
  offset += 4;
  wav.writeUInt32LE(SAMPLE_RATE * 2, offset);
  offset += 4;
  wav.writeUInt16LE(2, offset);
  offset += 2;
  wav.writeUInt16LE(16, offset);
  offset += 2;
  wav.write('data', offset);
  offset += 4;
  wav.writeUInt32LE(pcmBytes, offset);
  offset += 4;

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
  if (seconds < 60) return `${seconds}s ago`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m ago`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return `${hours}h ago`;
  const days = Math.floor(hours / 24);
  return `${days}d ago`;
}

async function ensureLatinScript(text, apiKey) {
  if (!containsNonLatinScript(text) || !apiKey) {
    return text;
  }

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
        {
          role: 'user',
          content: text,
        },
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

const styles = StyleSheet.create({
  screen: {
    flex: 1,
    backgroundColor: C.bg,
  },
  body: {
    flex: 1,
  },
  scroll: {
    paddingHorizontal: 22,
    paddingTop: 18,
    paddingBottom: 36,
  },

  gradTop: {
    position: 'absolute',
    top: 0,
    left: 0,
    right: 0,
    height: 120,
    backgroundColor: '#171B19',
    opacity: 0.55,
  },
  gradBottom: {
    position: 'absolute',
    bottom: 0,
    left: 0,
    right: 0,
    height: 200,
    backgroundColor: '#10130F',
    opacity: 0.7,
  },

  topBar: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: C.hairline,
    paddingBottom: 14,
    marginBottom: 22,
  },
  topBarLeft: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
  },
  topBarRight: {
    flexDirection: 'row',
    alignItems: 'center',
  },
  topMark: {
    color: C.accent,
    fontFamily: FONT.monoBd,
    fontSize: 11,
    letterSpacing: 2,
  },
  topMarkDim: {
    color: C.fgDim,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.4,
  },
  linkDot: {
    width: 10,
    height: 10,
    borderRadius: 1,
    transform: [{ rotate: '45deg' }],
  },

  hero: {
    paddingTop: 8,
    paddingBottom: 28,
  },
  heroLine1: {
    color: C.fg,
    fontFamily: FONT.display,
    fontSize: 52,
    letterSpacing: -2,
    lineHeight: 56,
  },
  heroLine2: {
    color: C.accent,
    fontFamily: FONT.display,
    fontSize: 52,
    letterSpacing: -2,
    lineHeight: 56,
    marginLeft: 18,
    marginTop: -6,
  },
  heroMetaRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    marginTop: 18,
    paddingTop: 10,
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: C.hairline,
  },
  heroMeta: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 11,
    letterSpacing: 1.4,
  },
  heroBars: {
    flexDirection: 'row',
    alignItems: 'flex-end',
    gap: 3,
    height: 18,
  },
  heroBar: {
    width: 3,
  },

  panel: {
    backgroundColor: C.surface,
    borderColor: C.hairline,
    borderWidth: 1,
    paddingHorizontal: 14,
    paddingTop: 10,
    paddingBottom: 14,
    marginBottom: 16,
  },
  panelHead: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingBottom: 10,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: C.hairlineSoft,
    marginBottom: 12,
  },
  panelLabel: {
    color: C.fgDim,
    fontFamily: FONT.monoMd,
    fontSize: 10,
    letterSpacing: 2.2,
    textTransform: 'uppercase',
  },
  panelMeta: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.4,
  },
  panelBody: {
    paddingTop: 2,
  },

  inputRow: {
    flexDirection: 'row',
    alignItems: 'center',
    backgroundColor: C.surfaceAlt,
    borderColor: C.hairlineSoft,
    borderWidth: 1,
  },
  input: {
    flex: 1,
    color: C.fg,
    fontFamily: FONT.mono,
    fontSize: 14,
    paddingHorizontal: 12,
    paddingVertical: 12,
    letterSpacing: 0.5,
  },
  inputAction: {
    paddingHorizontal: 12,
    paddingVertical: 12,
    borderLeftWidth: 1,
    borderLeftColor: C.hairlineSoft,
  },
  inputActionText: {
    color: C.accent,
    fontFamily: FONT.monoBd,
    fontSize: 11,
    letterSpacing: 2,
  },

  primary: {
    position: 'relative',
    backgroundColor: C.accent,
    paddingVertical: 18,
    paddingHorizontal: 18,
    marginTop: 4,
    marginBottom: 16,
  },
  primaryActive: {
    backgroundColor: C.surface,
    borderWidth: 1,
    borderColor: C.accent,
  },
  primaryPressed: {
    opacity: 0.85,
  },
  primaryDisabled: {
    backgroundColor: C.surface,
    borderWidth: 1,
    borderColor: C.hairline,
  },
  primaryEdge: {
    position: 'absolute',
    left: 6,
    right: 6,
    bottom: -5,
    height: 5,
    backgroundColor: C.accentDim,
  },
  primaryRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  primaryLabel: {
    color: C.bg,
    fontFamily: FONT.monoBd,
    fontSize: 14,
    letterSpacing: 3,
  },
  primaryLabelActive: {
    color: C.accent,
  },
  primaryGlyph: {
    color: C.bg,
    fontFamily: FONT.monoBd,
    fontSize: 14,
  },
  primaryGlyphActive: {
    color: C.accent,
  },

  channelRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingBottom: 10,
  },
  statRecBox: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
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
    fontSize: 12,
    letterSpacing: 2,
  },
  timer: {
    flex: 1,
    alignItems: 'center',
  },
  timerNum: {
    color: C.fg,
    fontFamily: FONT.monoBd,
    fontSize: 22,
    letterSpacing: 2,
  },
  signal: {
    flexDirection: 'row',
    alignItems: 'flex-end',
    gap: 2,
    height: 18,
  },
  signalBar: {
    width: 3,
    height: 18,
  },
  statusLine: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    paddingTop: 10,
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: C.hairlineSoft,
  },
  statusDot: {
    width: 6,
    height: 6,
    borderRadius: 3,
  },
  statusText: {
    color: C.fgDim,
    fontFamily: FONT.mono,
    fontSize: 12,
    letterSpacing: 0.6,
    flex: 1,
  },

  transcript: {
    position: 'relative',
    backgroundColor: C.surfaceAlt,
    borderColor: C.hairline,
    borderWidth: 1,
    paddingHorizontal: 18,
    paddingVertical: 14,
    minHeight: 220,
    marginBottom: 22,
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
    color: C.accent,
    fontFamily: FONT.monoBd,
    fontSize: 11,
    letterSpacing: 2.4,
    textTransform: 'uppercase',
  },
  transcriptMeta: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.4,
  },
  transcriptBody: {
    flexGrow: 0,
    paddingVertical: 16,
    maxHeight: 260,
  },
  transcriptBodyContent: {
    paddingRight: 4,
  },
  transcriptText: {
    color: C.fg,
    fontFamily: FONT.monoMd,
    fontSize: 22,
    lineHeight: 32,
    letterSpacing: 0.4,
  },
  transcriptPlaceholder: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 16,
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
    fontSize: 10,
    letterSpacing: 1.6,
  },

  archiveEmpty: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 12,
    letterSpacing: 1.4,
    paddingVertical: 6,
  },
  archiveList: {
    gap: 12,
  },
  archiveRow: {
    paddingTop: 10,
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: C.hairlineSoft,
  },
  archiveMeta: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingBottom: 6,
  },
  archiveIndex: {
    color: C.accent,
    fontFamily: FONT.monoBd,
    fontSize: 11,
    letterSpacing: 1.6,
  },
  archiveAge: {
    color: C.fgDim,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.2,
    flex: 1,
  },
  archiveDuration: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.2,
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
    borderColor: C.accent,
  },
  cornerTL: {
    top: -1,
    left: -1,
    borderTopWidth: 2,
    borderLeftWidth: 2,
  },
  cornerTR: {
    top: -1,
    right: -1,
    borderTopWidth: 2,
    borderRightWidth: 2,
  },
  cornerBL: {
    bottom: -1,
    left: -1,
    borderBottomWidth: 2,
    borderLeftWidth: 2,
  },
  cornerBR: {
    bottom: -1,
    right: -1,
    borderBottomWidth: 2,
    borderRightWidth: 2,
  },

  bottomRule: {
    height: 1,
    backgroundColor: C.hairline,
    marginBottom: 12,
  },
  legend: {
    gap: 6,
    paddingTop: 4,
  },
  legendRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
  },
  legendKey: {
    width: 22,
    height: 22,
    borderWidth: 1,
    borderColor: C.accent,
    alignItems: 'center',
    justifyContent: 'center',
  },
  legendKeyText: {
    color: C.accent,
    fontFamily: FONT.monoBd,
    fontSize: 11,
  },
  legendLabel: {
    color: C.muted,
    fontFamily: FONT.mono,
    fontSize: 10,
    letterSpacing: 1.6,
  },
});
