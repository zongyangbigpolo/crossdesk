import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../session/session_manager.dart';
import '../../state/providers.dart';

class SessionsPage extends ConsumerStatefulWidget {
  const SessionsPage({super.key});
  @override
  ConsumerState<SessionsPage> createState() => _SessionsPageState();
}

class _SessionsPageState extends ConsumerState<SessionsPage> {
  final _peerCtrl = TextEditingController();
  final _pwCtrl   = TextEditingController();

  @override
  void dispose() {
    _peerCtrl.dispose();
    _pwCtrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final sessions = ref.watch(sessionsProvider);
    final binPath = ref.watch(sessionBinaryProvider);

    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text('远程连接', style: Theme.of(context).textTheme.headlineMedium),
        const SizedBox(height: 4),
        Text('session 进程：$binPath', style: const TextStyle(color: Colors.grey, fontSize: 12)),
        const SizedBox(height: 16),
        Row(children: [
          SizedBox(
            width: 200,
            child: TextField(
              controller: _peerCtrl,
              decoration: const InputDecoration(labelText: '远端 ID', border: OutlineInputBorder()),
            ),
          ),
          const SizedBox(width: 8),
          SizedBox(
            width: 140,
            child: TextField(
              controller: _pwCtrl,
              obscureText: true,
              maxLength: 6,
              decoration: const InputDecoration(labelText: '密码', counterText: '', border: OutlineInputBorder()),
            ),
          ),
          const SizedBox(width: 12),
          FilledButton.icon(
            icon: const Icon(Icons.play_arrow),
            label: const Text('连接'),
            onPressed: () async {
              final id = _peerCtrl.text.trim();
              final pw = _pwCtrl.text.trim();
              if (id.isEmpty || pw.isEmpty) return;
              await ref.read(sessionsProvider.notifier).open(id, password: pw);
            },
          ),
        ]),
        const SizedBox(height: 24),
        Expanded(
          child: sessions.isEmpty
              ? const Center(child: Text('暂无活动会话', style: TextStyle(color: Colors.grey)))
              : ListView(children: sessions.values.map(_card).toList()),
        ),
      ]),
    );
  }

  Widget _card(SessionManager mgr) {
    final s = mgr.state;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Expanded(child: Text('peer ${s.peerId}', style: const TextStyle(fontWeight: FontWeight.w600))),
            _phaseChip(s.phase),
            const SizedBox(width: 8),
            IconButton(
              icon: const Icon(Icons.stop_circle_outlined),
              tooltip: '请求关闭',
              onPressed: () => ref.read(sessionsProvider.notifier).close(s.peerId),
            ),
          ]),
          const SizedBox(height: 4),
          Text('pid=${s.childPid ?? '-'}   endpoint=${s.endpoint ?? '-'}',
              style: const TextStyle(color: Colors.grey, fontSize: 12)),
          if (s.remoteState.isNotEmpty) Text('state: ${s.remoteState}'),
          if (s.lastError != null) Text('error: ${s.lastError}', style: const TextStyle(color: Colors.red)),
          if (s.stats.rttMs > 0 || s.stats.fps > 0)
            Text('rtt=${s.stats.rttMs}ms  fps=${s.stats.fps}  bitrate=${s.stats.bitrateKbps}kbps  loss=${s.stats.loss.toStringAsFixed(2)}'),
        ]),
      ),
    );
  }

  Widget _phaseChip(SessionPhase p) {
    final color = switch (p) {
      SessionPhase.running || SessionPhase.helloed => Colors.green,
      SessionPhase.failed                          => Colors.red,
      SessionPhase.exited                          => Colors.grey,
      _                                            => Colors.orange,
    };
    return Chip(
      label: Text(p.name, style: const TextStyle(color: Colors.white, fontSize: 11)),
      backgroundColor: color,
      padding: EdgeInsets.zero,
      visualDensity: VisualDensity.compact,
    );
  }
}
