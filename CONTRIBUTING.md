# Contributing

Thanks for taking the time to contribute.

## Before you start

- Read [README.md](README.md) for architecture, build and configuration details.
- Search existing issues before opening a new one.
- For a larger change, open an issue first so the approach can be discussed.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On Windows use a **Developer PowerShell** (MSVC is required because the CTP
SDK ships MSVC import libraries):

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The project currently has no automated tests. Please at least verify that the
project builds and that the service starts with
`config/MarketDataService.broadcast-only.json`.

## Code style

- C++11, no C++14/17-only features.
- Tabs for indentation (display width 4), Allman braces, UTF-8 source files.
- Keep the C++/C boundary of CTP callbacks exception-safe.
- Do not block CTP callback threads on disk or network I/O.
- Follow the existing `mdgw` namespace and naming conventions.

## Commit messages

Use short imperative subjects, preferably Conventional Commits:

```text
feat: add multicast TTL option
fix: flush csv queue before releasing the CTP api
docs: describe the local csv rotation
```

Keep unrelated changes in separate commits.

## Sensitive data

Never commit:

- real CTP / ClickHouse accounts or passwords,
- production server addresses,
- `config/instruments_cache.json`,
- logs, CSV tick data, or CTP flow files.

Use the placeholder values in `config/` and keep your real configuration
outside the repository.

## License

By contributing, you agree that your contributions are licensed under the
project's MIT License (see [LICENSE](LICENSE)).
