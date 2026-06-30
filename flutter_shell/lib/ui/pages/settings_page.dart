import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../core/core.dart';
import '../../state/providers.dart';

class SettingsPage extends ConsumerWidget {
  const SettingsPage({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    ref.watch(configRevProvider);
    final core = ref.watch(coreProvider);

    void bump() => ref.read(configRevProvider.notifier).state++;

    return ListView(
      padding: const EdgeInsets.all(24),
      children: [
        Text('设置（M3 最小切片）', style: Theme.of(context).textTheme.headlineSmall),
        const SizedBox(height: 16),

        _section('通用', [
          _enumDropdown<Language>('语言', core.language, Language.values, (v) {
            core.setLanguage(v); bump();
          }),
          _switchTile('开机自启', core.autostart, (v) { core.setAutostart(v); bump(); }),
          _switchTile('最小化到托盘', core.minimizeToTray, (v) { core.setMinimizeToTray(v); bump(); }),
        ]),

        _section('视频', [
          _enumDropdown<VideoQuality>('画质', core.videoQuality, VideoQuality.values, (v) {
            core.setVideoQuality(v); bump();
          }),
          _enumDropdown<VideoFps>('帧率', core.videoFps, VideoFps.values, (v) {
            core.setVideoFps(v); bump();
          }),
          _enumDropdown<VideoCodec>('编码', core.videoCodec, VideoCodec.values, (v) {
            core.setVideoCodec(v); bump();
          }),
          _switchTile('硬件编解码', core.hardwareCodec, (v) { core.setHardwareCodec(v); bump(); }),
        ]),

        _section('网络', [
          _switchTile('启用 TURN', core.turnEnabled, (v) { core.setTurn(v); bump(); }),
          _switchTile('启用 SRTP', core.srtpEnabled, (v) { core.setSrtp(v); bump(); }),
          _switchTile('自建信令', core.selfHosted, (v) { core.setSelfHosted(v); bump(); }),
        ]),

        const SizedBox(height: 24),
        const Text(
          '验证方式：改任意设置 → 切到主页查看 → 重启进程后再回来，值应当被持久化。',
          style: TextStyle(color: Colors.grey),
        ),
      ],
    );
  }

  Widget _section(String title, List<Widget> children) => Card(
        margin: const EdgeInsets.only(bottom: 16),
        child: Padding(
          padding: const EdgeInsets.all(8),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(8, 8, 8, 4),
              child: Text(title, style: const TextStyle(fontWeight: FontWeight.w600)),
            ),
            ...children,
          ]),
        ),
      );

  Widget _switchTile(String label, bool v, ValueChanged<bool> onChanged) =>
      SwitchListTile(title: Text(label), value: v, onChanged: onChanged, dense: true);

  Widget _enumDropdown<T extends Enum>(
          String label, T current, List<T> options, ValueChanged<T> onChanged) =>
      ListTile(
        title: Text(label),
        trailing: DropdownButton<T>(
          value: current,
          onChanged: (v) { if (v != null) onChanged(v); },
          items: options.map((e) => DropdownMenuItem(value: e, child: Text(e.name))).toList(),
        ),
        dense: true,
      );
}
