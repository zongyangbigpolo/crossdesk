import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'ui/pages/home_page.dart';
import 'ui/pages/sessions_page.dart';
import 'ui/pages/settings_page.dart';

void main() {
  runApp(const ProviderScope(child: CrossDeskApp()));
}

class CrossDeskApp extends StatelessWidget {
  const CrossDeskApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'CrossDesk',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
        useMaterial3: true,
      ),
      home: const _RootShell(),
    );
  }
}

class _RootShell extends StatefulWidget {
  const _RootShell();
  @override
  State<_RootShell> createState() => _RootShellState();
}

class _RootShellState extends State<_RootShell> {
  int _index = 0;
  static const _pages = [HomePage(), SessionsPage(), SettingsPage()];

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Row(children: [
        NavigationRail(
          selectedIndex: _index,
          onDestinationSelected: (i) => setState(() => _index = i),
          labelType: NavigationRailLabelType.all,
          destinations: const [
            NavigationRailDestination(icon: Icon(Icons.home_outlined),     selectedIcon: Icon(Icons.home),     label: Text('主页')),
            NavigationRailDestination(icon: Icon(Icons.cast_outlined),     selectedIcon: Icon(Icons.cast),     label: Text('远程')),
            NavigationRailDestination(icon: Icon(Icons.settings_outlined), selectedIcon: Icon(Icons.settings), label: Text('设置')),
          ],
        ),
        const VerticalDivider(width: 1),
        Expanded(child: _pages[_index]),
      ]),
    );
  }
}
