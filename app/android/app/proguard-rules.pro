# Flutter / Dart is compiled to AOT native code — keep the generated shims.
-keep class io.flutter.app.** { *; }
-keep class io.flutter.plugin.**  { *; }
-keep class io.flutter.util.**  { *; }
-keep class io.flutter.view.**  { *; }
-keep class io.flutter.**  { *; }
-keep class io.flutter.plugins.**  { *; }

# flutter_blue_plus — BLE scanning/connecting uses native + callback receivers.
-keep class com.lib.flutter_blue_plus.** { *; }

# permission_handler — permission status callbacks are dispatched reflectively.
-keep class com.baseflow.permissionhandler.** { *; }

# path_provider — method channel host that returns file paths.
-keep class io.flutter.plugins.pathprovider.** { *; }

# file_picker — platform channels + Pigeon-generated code.
-keep class com.mr.flutter.plugin.filepicker.** { *; }
-keep class fr.skyost.pigeon.** { *; }

# Flutter engine's deferred (split-install) component references Play Core
# classes that are absent unless the Play Core SDK is bundled. The app does not
# use Play Store dynamic feature delivery, so keep the engine's manager intact
# and silence the unresolved-reference warnings (standard Flutter + R8 setup).
-dontwarn com.google.android.play.core.**
-keep class io.flutter.embedding.engine.deferredcomponents.** { *; }
