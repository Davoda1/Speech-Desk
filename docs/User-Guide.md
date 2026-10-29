# Using SpeechDesk

## Compose text

Type or paste into the editor, or choose **Open TXT…** for a UTF-8 `.txt` or `.md` file. Markdown is read as plain text. File input is limited to 2 MiB; a reading may contain up to 200,000 characters. **Save text…** writes the editor contents as UTF-8 TXT. Text is not saved automatically.

Choose **Speak** to read all text, or select a passage first. Text used by an active task is protected from simultaneous edits. Speech is split at sentence and paragraph boundaries; long sentences are divided at the profile's section limit. The default is 500 characters. Section boundaries may affect phrasing.

## Listen and save audio

**Pause/Resume** controls local playback. **Stop** ends playback and cancels pending client work. Pausing playback does not pause remote generation. Remote services may continue accepted requests after cancellation.

The position slider seeks within the current section; volume changes playback volume. After completion, Speak can reuse the cached reading while its text, selection and effective profile are unchanged. Starting another reading replaces the cache.

**Save audio…** becomes available after all sections have completed. It combines the reading into one WAV file using 48 kHz, mono, 16-bit PCM. Save before closing the app to keep the audio. Conversion requires a working decoder and reports an error if the required format is unavailable.

Word highlighting uses supported word timings returned by the service and matched to the requested text. Without those timings, the currently playing sentence or section is highlighted. There is no estimated moving word clock. Highlighting and following the current passage can be changed in **Settings → Appearance**.

## Dictate locally

Place the cursor or select text to replace, then choose **Dictate**. Select a model, language and microphone. The refresh button updates the device list. Microphone access must be allowed by the operating system.

With **Live text** enabled, recognition periodically updates provisional words in the editor. **Stop recording** finishes capture and produces the final text from the recording. Provisional words may change. With Live text disabled, recognition begins after recording stops.

**Cancel** restores the text and selection from before dictation. Errors and an empty final result also preserve the original text. Successful insertion can be undone once with Ctrl+Z. Dictation never starts speech playback automatically. Playback and dictation cannot run simultaneously.

Live text depends on available CPU capacity and is not guaranteed to keep up with speech. For larger models or slower computers, turn Live text off and use record-then-recognize. Recognition CPU threads can be adjusted in Settings.

**Audio file…** accepts WAV, MP3 and FLAC, up to one hour and 1 GiB. Recognition inserts the result at the same cursor/selection position. Edit or save the resulting text, or press Speak.

## Recognition models

| Included model | Languages / purpose |
| --- | --- |
| Whistle | Compact recognition for English, German, French, Spanish, Italian, Dutch and Polish |
| Whisper Tiny Q5_1 | Small multilingual model, including Hungarian |
| Whisper Base Q5_1 | Multilingual alternative |
| Whisper Small Q5_1 | Larger multilingual alternative |

The four model files total approximately 299 MB. Recognition runs locally on the CPU. Accuracy and speed depend on the recording, language, model and hardware. [Optional model packs](Optional-Models.md) add the two larger Whisper choices.

## Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| Ctrl+O | Open text |
| Ctrl+S | Save text |
| Ctrl+Shift+S | Save completed speech audio |
| Ctrl+Enter | Speak or pause/resume |
| Ctrl+R | Open dictation controls and start/stop recording |
| Escape | Cancel the active task |

## Preferences and cleanup

Settings and installed models use the `Speech-Desk` directory in the user's configuration location. API keys are session-only. Model import is independent of the Settings dialog's Save/Cancel buttons: a successfully imported model remains installed.

Temporary playback and recognition files are removed during normal cleanup. A forced process termination can leave operating-system temporary files behind. Keep anything you want to retain by using Save text or Save audio.
