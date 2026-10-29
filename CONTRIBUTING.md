# Contributing

Use `master` as the target branch. Keep changes focused and explain the concrete problem and resulting behavior in the commit or pull-request title and description. Include the checks actually run and any relevant limitations.

Build and run the focused tests described in [README](README.md). Provider changes should include deterministic request/response fixtures where practical. Normal CI must not require paid API requests or real credentials. Physical-device checks should be described separately from fixture tests.

Keep source files free of generated build output, runtime models, credentials, personal configuration and machine-specific paths. Preserve dependency notices and pinned source information. New settings must have safe defaults; speech-to-text must remain local unless a separately reviewed feature explicitly changes that behavior.

For reports, include the operating system, SpeechDesk version, reproducible steps and redacted diagnostics. Use synthetic text or audio. Follow [SECURITY.md](SECURITY.md) for security-sensitive reports.

Contributions to SpeechDesk application code are provided under the project's [MIT license](LICENSE.txt). Third-party components retain their own licenses.
