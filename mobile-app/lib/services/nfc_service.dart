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
            final ndef = NdefAndroid.from(tag);
            if (ndef == null || !ndef.isWritable) {
              debugPrint('NFC: Tag is not writable');
              await NfcManager.instance.stopSession();
              onError?.call();
              return;
            }

            final jsonData = json.encode({
              'id': filament.id,
              'manufacturer': filament.manufacturer,
              'material': filament.material,
              'density': filament.density,
              'weight': filament.weight,
              'spoolWeight': filament.spoolWeight,
              'diameter': filament.diameter,
              'bedTemp': filament.bedTemp,
            });

            // Создаём текстовую запись
            final languageCode = 'en';
            final languageCodeBytes = Uint8List.fromList(languageCode.codeUnits);
            final textBytes = Uint8List.fromList(utf8.encode(jsonData));
            final payload = Uint8List(1 + languageCodeBytes.length + textBytes.length);
            payload[0] = languageCodeBytes.length;
            payload.setRange(1, 1 + languageCodeBytes.length, languageCodeBytes);
            payload.setRange(1 + languageCodeBytes.length, payload.length, textBytes);

            final message = NdefMessage(
              records: [
                NdefRecord(
                  typeNameFormat: TypeNameFormat.wellKnown,
                  type: Uint8List.fromList([0x54]), // 'T' for Text
                  identifier: Uint8List(0),
                  payload: payload,
                ),
              ],
            );

            await ndef.writeNdefMessage(message);
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
