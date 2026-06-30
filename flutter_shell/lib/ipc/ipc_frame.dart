// Wire format: [4-byte big-endian uint32 length][UTF-8 JSON payload].
// Mirrors src/ipc/ipc_frame.h on the C++ side. Keep in sync.

import 'dart:convert';
import 'dart:typed_data';

const int kMaxFrameBytes = 16 * 1024 * 1024;

Uint8List encodeFrame(Map<String, dynamic> json) {
  final payload = utf8.encode(jsonEncode(json));
  if (payload.length > kMaxFrameBytes) {
    throw ArgumentError('payload exceeds kMaxFrameBytes');
  }
  final out = BytesBuilder(copy: false);
  final hdr = ByteData(4)..setUint32(0, payload.length, Endian.big);
  out.add(hdr.buffer.asUint8List());
  out.add(payload);
  return out.toBytes();
}

enum DecodeStatus { needMore, ok, oversize, malformed }

class DecodeResult {
  DecodeResult(this.status, {this.payload, this.consumed = 0});
  final DecodeStatus status;
  final Map<String, dynamic>? payload;
  final int consumed;
}

DecodeResult tryDecodeFrame(Uint8List buf) {
  if (buf.length < 4) return DecodeResult(DecodeStatus.needMore);
  final len = ByteData.sublistView(buf, 0, 4).getUint32(0, Endian.big);
  if (len > kMaxFrameBytes) return DecodeResult(DecodeStatus.oversize);
  if (buf.length < 4 + len) return DecodeResult(DecodeStatus.needMore);
  try {
    final json = jsonDecode(utf8.decode(buf.sublist(4, 4 + len)));
    if (json is! Map<String, dynamic>) {
      return DecodeResult(DecodeStatus.malformed, consumed: 4 + len);
    }
    return DecodeResult(DecodeStatus.ok, payload: json, consumed: 4 + len);
  } catch (_) {
    return DecodeResult(DecodeStatus.malformed, consumed: 4 + len);
  }
}
