# FruitPi

ESP32-S3 firmware + Flutter companion app.

## Firmware (esp/)
```bash
cd esp
pio run -t compiledb   # generates compile_commands.json for LSP
pio run                # builds & flashes
```

The LSP (clangd) auto-configures via `.clangd` + `extra_script.py` — no manual setup needed.

## Flutter App (app/)
```bash
cd app
flutter pub get        # fetch dependencies
flutter run            # run on connected device/emulator
```

Requires Flutter SDK in PATH. No other setup required.