import 'dart:convert';
import 'dart:typed_data';
import 'package:flutter/foundation.dart';
import 'package:nfc_manager/nfc_manager.dart';
import 'package:nfc_manager/nfc_manager_android.dart';
import 'package:nfc_manager/ndef_record.dart';
import '../models/filament.dart';

class NfcService {
  static Future<bool> isAvailable() async {
    final availability = await NfcManager.instance.checkAvailability();
    return availability == NfcAvailability.enabled;
  }

  static Future<void> writeFilamentToTag(
    Filament filament, {
    Function? onSuccess,
    Function? onError,
  }) async {
    try {
      await NfcManager.instance.startSession(
        pollingOptions: {NfcPollingOption.iso14443, NfcPollingOption.iso15693},
        onDiscovered: (NfcTag tag) async {
          try {
            // ИСПРАВЛЕНИЕ: Пишем только ID в raw формат (совместимо с ESP32)
            // ESP32 читает блок 4 как raw 16 байт
            final ndef = NdefAndroid.from(tag);
            if (ndef == null || !ndef.isWritable) {
              debugPrint('NFC: Tag is not writable');
              await NfcManager.instance.stopSession();
              onError?.call();
              return;
            }

            // Создаём простую текстовую запись с ID (максимум 16 байт)
            final idBytes = utf8.encode(filament.id);
            final payload = Uint8List(16);
            // Копируем ID в payload (максимум 16 байт)
            final len = idBytes.length > 16 ? 16 : idBytes.length;
            payload.setRange(0, len, idBytes);

            final message = NdefMessage(
              records: [
                NdefRecord(
                  typeNameFormat: TypeNameFormat.wellKnown,
                  type: Uint8List.fromList([0x54]), // 'T' for Text
                  identifier: Uint8List(0),
                  payload: Uint8List.fromList([0x02, 0x65, 0x6E, ...payload]), // 0x02 = UTF-8, 'en'
                ),
              ],
            );

            await ndef.writeNdefMessage(message);
            debugPrint('NFC: Written ID: ${filament.id}');
            await NfcManager.instance.stopSession();
            onSuccess?.call();
          } catch (e) {
            debugPrint('NFC write error: $e');
            await NfcManager.instance.stopSession();
            onError?.call();
          }
        },
      );
    } catch (e) {
      debugPrint('NFC error: $e');
      onError?.call();
    }
  }

  static Future<void> stopSession() async {
    await NfcManager.instance.stopSession();
  }
}
