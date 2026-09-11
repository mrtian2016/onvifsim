# Contributing

Thanks for looking. The project language is Chinese for code comments and design docs,
English for `README.md` and anything user-facing in English builds. Issues and pull
requests are fine in either language.

## The most useful contribution

**A description of how a real camera misbehaves.** That is what this project is made of.

Every one of the 90 fault-injection switches exists because some device actually behaved
that way and some client actually broke on it. If you have a camera that does something a
client cannot handle, and onvifsim cannot reproduce it yet, open an issue with:

- make, model, firmware version;
- what the device sends (a packet capture, a SOAP body, an RTSP exchange — trimmed is
  fine, redact serial numbers and hostnames);
- what the client does about it.

That is enough for a new quirk. You do not have to write the code.

## Adding a quirk

Quirks are the one part of the codebase with a hard checklist, because a half-added quirk
is worse than none — the table in `docs/quirks.md` is generated from code and is treated
as the source of truth by both the GUI and the tests.

A new quirk needs **all** of:

1. An entry in the table in `src/core/Quirks.cpp` — key, group, source ID, title,
   description, parameters.
2. The behavior itself, wherever the protocol is produced.
3. A checkbox in the GUI (`src/gui/tabs/QuirksTab.cpp` builds from the table, so this is
   usually free).
4. **An end-to-end assertion** in `tests/e2e/` that the switch actually changes behavior.
   `tests/e2e/test_quirk_coverage.py` fails if any switch has no test.
5. Regenerated docs: `scripts/gen-docs.sh`. CI checks that `docs/quirks.md` matches the
   code.

Do not invent quirks. Each one carries a source ID pointing at the observation behind it;
if there is no observation, there is no quirk.

## Ground rules for code

These are load-bearing. Please do not relax them without discussing it first:

- **No third-party libraries.** XML, HTTP, RTSP, RTP, Digest auth and JPEG are all
  hand-written on Qt. No ffmpeg, no Python, no Java at runtime.
- **The source must compile against Qt 6.2 and CMake 3.21**, so distro Qt works. In
  particular: no `QHttpServer` (it was a preview module before 6.4).
- **The GUI never touches protocol details.** It observes `Simulator` through signals.
  Headless builds must keep compiling without Qt Widgets.
- All timestamps are UTC, and must honour the injected clock skew.
- Response XML comes from the parameterised writers in `soap/XmlWriter`, not from a DOM
  tree — several quirks require deliberately malformed output.

## Before you open a pull request

```bash
cmake --preset conda-linux -DENABLE_WERROR=ON
cmake --build --preset conda-linux
ctest --preset conda-linux

cd tests/e2e && .venv/bin/python -m pytest -q \
  --onvifsim-binary=../../build/conda-linux/bin/onvifsim
```

`-Werror` is on in CI on all three platforms, and the build is expected to be warning-free.

If you touched the GUI, also run `tests/e2e/test_gui_stream.py`. The protocol tests run
headless and never execute a line of interface code, which has hidden a crash before.

## Style

`.editorconfig` covers it: four spaces for C++, two for CMake/JSON/YAML/Markdown, LF,
UTF-8, newline at end of file. Naming is `PascalCase` types, `m_` members, `k` constants,
`enum class`; the codebase is consistent about this, so follow what is around you.

Comments explain **why**, in Chinese. A comment that restates the code is worse than no
comment. If you are adding a workaround, say what breaks without it.

## Translations

Three kinds of strings, three rules:

| Kind | Rule |
|---|---|
| GUI labels and buttons | Follow the UI language, via `tr()` |
| Core data tables (persona names, quirk titles/descriptions) | Follow the UI language, looked up through `gui/I18n.h` |
| Error messages that reach a dialog | Follow the UI language, via `QCoreApplication::translate("onvifsim::core", …)` |
| **Log summaries** | **Always English.** Do not write Chinese into a log line |

Logs stay English on purpose: they end up in files, on stdout and on the REST SSE stream.
A log whose language depends on the reader's locale is a log that cannot be pasted into
an issue, and text-matching automation on top of it is not reliable.

After adding `tr()` calls or changing the quirk table:

```bash
tools/make-i18n.py                                  # only if you touched the quirk table
lupdate6 -I src src -locations none -no-obsolete \
    -ts assets/i18n/onvifsim_zh_CN.ts assets/i18n/onvifsim_en.ts
```

Scan all of `src`, not just `src/gui`: the quirk table's strings live in a generated
`src/core/QuirkStrings.cpp`, and a narrower scan makes lupdate delete them as obsolete.
`tst_i18n` fails if any table string lacks an English translation, so a forgotten step
shows up as a test failure rather than as Chinese text in the English UI.

Do not edit `.ts` files with regular expressions — a greedy match across `<message>`
boundaries corrupts the XML. Use an XML parser.
