import 'package:go_router/go_router.dart';
import 'package:llms_example/home_screen.dart';

class GoRouterConfig {
  GoRouterConfig._();

  static GoRouter get router => GoRouter(
    initialLocation: '/',
    redirect: (context, state) => state.path,
    routes: [
      GoRoute(
        path: '/',
        name: 'home',
        builder: (context, state) => HomeScreen(),
      ),
    ],
  );
}
