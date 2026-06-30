import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../state/providers.dart';

class HomePage extends ConsumerWidget {
  const HomePage({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    ref.watch(configRevProvider); // re-read after writes
    final core = ref.watch(coreProvider);

    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('CrossDesk', style: Theme.of(context).textTheme.headlineMedium),
          Text('core ${core.version}', style: Theme.of(context).textTheme.bodySmall),
          const SizedBox(height: 24),
          Card(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('当前配置（来自 libcrossdesk_core）',
                      style: TextStyle(fontWeight: FontWeight.w600)),
                  const SizedBox(height: 12),
                  _row('Language',       core.language.name),
                  _row('Video quality',  core.videoQuality.name),
                  _row('Video FPS',      core.videoFps.name),
                  _row('Video codec',    core.videoCodec.name),
                  _row('Hardware codec', core.hardwareCodec.toString()),
                  _row('TURN enabled',   core.turnEnabled.toString()),
                  _row('Self-hosted',    core.selfHosted.toString()),
                  _row('Signal host',    core.signalHost.isEmpty ? '(default)' : core.signalHost),
                  _row('Signal port',    core.signalPort.toString()),
                  _row('File save path', core.fileSavePath.isEmpty ? '(default)' : core.fileSavePath),
                ],
              ),
            ),
          ),
          const SizedBox(height: 12),
          const Text(
            '远程连接面板、最近连接、文件传输等将在 M3 后续 slice 加上。\n'
            '本页验证 Dart ↔ C++ ABI 端到端读通。',
            style: TextStyle(color: Colors.grey),
          ),
        ],
      ),
    );
  }

  Widget _row(String k, String v) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 4),
        child: Row(children: [
          SizedBox(width: 160, child: Text(k, style: const TextStyle(color: Colors.grey))),
          Expanded(child: Text(v)),
        ]),
      );
}
