# Development rules

- Preserve the original MIT license and third-party notices.
- Read docs/BASELINE.md before modifying the mod. mod/ currently pairs with vendor/v0.22/dxgi.dll only.
- Do not mix the v0.22 and v1.1.0 Lua/native protocols.
- Never commit local game files, user settings, logs or reference material under local/.
- Keep development separate from the live game installation. Back up files before an explicitly requested deployment.
- Report unverified VR behavior honestly; a clean log is not a headset comfort test.
- Validate projection and tracking without requiring repeated headset tests from a user who reported nausea.
