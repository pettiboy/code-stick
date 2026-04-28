import { StatusBar } from 'expo-status-bar';
import * as SecureStore from 'expo-secure-store';
import { Buffer } from 'buffer';
import { useEffect, useMemo, useRef, useState } from 'react';
import {
  ActivityIndicator,
  Alert,
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

const DEVICE_NAME = 'M5VoiceStick';
const SERVICE_UUID = '3e7a0001-e33b-4e2f-9a85-f03e1d33c001';
const AUDIO_UUID = '3e7a0002-e33b-4e2f-9a85-f03e1d33c001';
const CONTROL_UUID = '3e7a0003-e33b-4e2f-9a85-f03e1d33c001';
const SAMPLE_RATE = 16000;
const API_KEY_STORAGE_KEY = 'openai_api_key';

export default function App() {
  const manager = useMemo(() => new BleManager(), []);
  const [apiKey, setApiKey] = useState('');
  const [device, setDevice] = useState(null);
  const [status, setStatus] = useState('Ready');
  const [isScanning, setIsScanning] = useState(false);
  const [isConnected, setIsConnected] = useState(false);
  const [isRecording, setIsRecording] = useState(false);
  const [isTranscribing, setIsTranscribing] = useState(false);
  const [transcript, setTranscript] = useState('Transcript appears here.');
  const audioChunksRef = useRef([]);
  const controlLineRef = useRef('');
  const subscriptionsRef = useRef([]);
  const deviceRef = useRef(null);
  const isScanningRef = useRef(false);

  useEffect(() => {
    SecureStore.getItemAsync(API_KEY_STORAGE_KEY).then((stored) => {
      if (stored) setApiKey(stored);
    });

    return () => {
      stopScan();
      subscriptionsRef.current.forEach((subscription) => subscription.remove());
      manager.destroy();
    };
  }, [manager]);

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
      Alert.alert('API key required', 'Add an OpenAI API key before connecting.');
      return;
    }

    const permitted = await requestBluetoothPermissions();
    if (!permitted) {
      setStatus('Bluetooth permission denied');
      return;
    }

    setTranscript('Transcript appears here.');
    setStatus(`Scanning for ${DEVICE_NAME}`);
    setIsScanning(true);
    isScanningRef.current = true;

    manager.startDeviceScan([SERVICE_UUID], { allowDuplicates: false }, async (error, scannedDevice) => {
      if (error) {
        setStatus(error.message);
        stopScan();
        return;
      }

      if (!scannedDevice) {
        return;
      }

      stopScan();
      try {
        setStatus('Connecting');
        const connected = await scannedDevice.connect({ requestMTU: 247 });
        const ready = await connected.discoverAllServicesAndCharacteristics();
        deviceRef.current = ready;
        setDevice(ready);
        setIsConnected(true);
        setStatus('Connected');
        monitorStick(ready);
        await sendControl('STATE:Ready\n', ready);
      } catch (connectError) {
        setStatus(connectError.message);
        setIsConnected(false);
      }
    });

    setTimeout(() => {
      if (isScanningRef.current) {
        stopScan();
        setStatus('Scan timed out');
      }
    }, 12000);
  }

  function stopScan() {
    manager.stopDeviceScan();
    isScanningRef.current = false;
    setIsScanning(false);
  }

  async function disconnect() {
    stopScan();
    subscriptionsRef.current.forEach((subscription) => subscription.remove());
    subscriptionsRef.current = [];
    if (deviceRef.current) {
      await deviceRef.current.cancelConnection().catch(() => {});
    }
    deviceRef.current = null;
    setDevice(null);
    setIsConnected(false);
    setIsRecording(false);
    setStatus('Disconnected');
  }

  function monitorStick(connectedDevice) {
    const controlSub = connectedDevice.monitorCharacteristicForService(
      SERVICE_UUID,
      CONTROL_UUID,
      (error, characteristic) => {
        if (error) {
          setStatus(error.message);
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
          setStatus(error.message);
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
      setTranscript('');
      setIsRecording(true);
      setStatus('Recording from Stick');
      sendControl('STATE:Recording\n');
      return;
    }

    if (line === 'STOP') {
      const chunks = audioChunksRef.current.slice();
      audioChunksRef.current = [];
      setIsRecording(false);
      transcribeChunks(chunks);
    }
  }

  async function transcribeChunks(chunks) {
    if (!chunks.length) {
      setStatus('No audio received');
      await sendControl('ERR:No audio\n');
      return;
    }

    try {
      setIsTranscribing(true);
      setStatus('Uploading to OpenAI');
      await sendControl('STATE:Transcribing\n');

      const wav = buildWav(chunks);
      const wavBytes = wav.buffer.slice(wav.byteOffset, wav.byteOffset + wav.byteLength);
      const wavBlob = new Blob([wavBytes], { type: 'audio/wav' });

      const form = new FormData();
      form.append('model', 'gpt-4o-mini-transcribe');
      form.append('response_format', 'json');
      form.append('file', wavBlob, 'm5-stick.wav');

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

      const text = payload.text?.trim() || '(No speech detected)';
      setTranscript(text);
      setStatus('Transcript sent to Stick');
      await sendControl(`TEXT:${sanitizeControlText(text)}\n`);
    } catch (error) {
      setStatus(error.message);
      await sendControl(`ERR:${sanitizeControlText(error.message)}\n`);
    } finally {
      setIsTranscribing(false);
    }
  }

  async function sendControl(message, targetDevice = deviceRef.current || device) {
    if (!targetDevice) return;

    const bytes = Buffer.from(message, 'utf8');
    for (let offset = 0; offset < bytes.length; offset += 160) {
      const chunk = bytes.subarray(offset, offset + 160);
      await targetDevice.writeCharacteristicWithResponseForService(
        SERVICE_UUID,
        CONTROL_UUID,
        chunk.toString('base64'),
      );
    }
  }

  return (
    <SafeAreaView style={styles.screen}>
      <StatusBar style="dark" />
      <View style={styles.header}>
        <Text style={styles.title}>M5 Voice Stick</Text>
        <View style={[styles.badge, isConnected ? styles.badgeOn : styles.badgeOff]}>
          <Text style={styles.badgeText}>{isConnected ? 'BLE On' : 'BLE Off'}</Text>
        </View>
      </View>

      <View style={styles.panel}>
        <Text style={styles.label}>OpenAI API Key</Text>
        <TextInput
          value={apiKey}
          onChangeText={saveApiKey}
          placeholder="sk-..."
          autoCapitalize="none"
          autoCorrect={false}
          secureTextEntry
          style={styles.input}
        />
        <View style={styles.actions}>
          <Pressable
            disabled={isScanning || isTranscribing}
            onPress={isConnected ? disconnect : connect}
            style={({ pressed }) => [
              styles.button,
              isConnected ? styles.secondaryButton : styles.primaryButton,
              pressed && styles.pressed,
              (isScanning || isTranscribing) && styles.disabled,
            ]}
          >
            <Text style={isConnected ? styles.secondaryButtonText : styles.primaryButtonText}>
              {isConnected ? 'Disconnect' : 'Connect'}
            </Text>
          </Pressable>
        </View>
      </View>

      <View style={styles.statusRow}>
        {(isScanning || isRecording || isTranscribing) && <ActivityIndicator color="#006C67" />}
        <Text style={styles.status}>{status}</Text>
      </View>

      <ScrollView style={styles.transcriptPanel} contentContainerStyle={styles.transcriptContent}>
        <Text style={styles.transcript}>{transcript}</Text>
      </ScrollView>
    </SafeAreaView>
  );
}

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

const styles = StyleSheet.create({
  screen: {
    flex: 1,
    backgroundColor: '#F7F8F5',
    paddingHorizontal: 20,
  },
  header: {
    alignItems: 'center',
    flexDirection: 'row',
    justifyContent: 'space-between',
    paddingBottom: 18,
    paddingTop: 18,
  },
  title: {
    color: '#17201D',
    fontSize: 28,
    fontWeight: '700',
  },
  badge: {
    borderRadius: 999,
    paddingHorizontal: 12,
    paddingVertical: 6,
  },
  badgeOn: {
    backgroundColor: '#D8F2E6',
  },
  badgeOff: {
    backgroundColor: '#E5E7EB',
  },
  badgeText: {
    color: '#17201D',
    fontSize: 13,
    fontWeight: '700',
  },
  panel: {
    backgroundColor: '#FFFFFF',
    borderColor: '#DBE2DD',
    borderRadius: 8,
    borderWidth: 1,
    padding: 14,
  },
  label: {
    color: '#4B5A55',
    fontSize: 13,
    fontWeight: '700',
    marginBottom: 8,
    textTransform: 'uppercase',
  },
  input: {
    backgroundColor: '#F2F4F1',
    borderColor: '#CBD4CF',
    borderRadius: 6,
    borderWidth: 1,
    color: '#17201D',
    fontSize: 16,
    paddingHorizontal: 12,
    paddingVertical: 12,
  },
  actions: {
    flexDirection: 'row',
    marginTop: 12,
  },
  button: {
    alignItems: 'center',
    borderRadius: 6,
    justifyContent: 'center',
    minHeight: 48,
    paddingHorizontal: 18,
    width: '100%',
  },
  primaryButton: {
    backgroundColor: '#006C67',
  },
  secondaryButton: {
    backgroundColor: '#FFFFFF',
    borderColor: '#006C67',
    borderWidth: 1,
  },
  primaryButtonText: {
    color: '#FFFFFF',
    fontSize: 16,
    fontWeight: '700',
  },
  secondaryButtonText: {
    color: '#006C67',
    fontSize: 16,
    fontWeight: '700',
  },
  pressed: {
    opacity: 0.75,
  },
  disabled: {
    opacity: 0.45,
  },
  statusRow: {
    alignItems: 'center',
    flexDirection: 'row',
    gap: 10,
    minHeight: 56,
  },
  status: {
    color: '#43514C',
    flex: 1,
    fontSize: 15,
  },
  transcriptPanel: {
    backgroundColor: '#FFFFFF',
    borderColor: '#DBE2DD',
    borderRadius: 8,
    borderWidth: 1,
    flex: 1,
    marginBottom: 20,
  },
  transcriptContent: {
    padding: 18,
  },
  transcript: {
    color: '#17201D',
    fontSize: 24,
    lineHeight: 34,
  },
});
