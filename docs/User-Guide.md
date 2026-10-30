# Using SpeechDesk

## Compose text

Type or paste into the editor, or choose **Open TXT…** for a UTF-8 `.txt` or `.md` file. Markdown is read as plain text. File input is limited to 2 MiB; a reading may contain up to 200,000 characters. **Save text…** writes the editor contents as UTF-8 TXT. Text is not saved automatically.

Choose **Speak** to read all text, or select a passage first. Text used by an active task is protected from simultaneous edits. Speech is split at sentence and paragraph boundaries; long sentences are divided at the profile's section limit. The default is 500 characters. Section boundaries may affect phrasing.

## Listen and save audio

**Pause/Resume** controls local playback. **Stop** ends playback and cancels pending client work. Pausing playback does not pause remote generation. Remote services may continue accepted requests after cancellation.

The position slider seeks within the current section; volume changes playback volume. **Reading part N of M** shows the currently playing part and the total part count. **Paused · part N of M** is shown when paused. The slider and time display apply to that part, not to the entire reading; the part count is hidden while idle. After completion, Speak can reuse the cached reading while its text, selection and effective profile are unchanged. Starting another reading replaces the cache.

**Save audio…** becomes available after all sections have completed. It combines the reading into one WAV file using 48 kHz, mono, 16-bit PCM. Save before closing the app to keep the audio. Conversion requires a working decoder and reports an error if the required format is unavailable.

OpenAI Realtime buffers the complete section and checks its returned final transcript against the original text before playback. Only letter case, whitespace, and equivalent Unicode representation are normalized. Missing or mismatched transcripts, including punctuation or numeric-format differences, reject that section with an error. See [server profiles](Server-Profiles.md#openai-realtime).

Word highlighting uses supported word timings returned by the service and matched to the requested text. Without those timings, the currently playing sentence or section is highlighted. There is no estimated moving word clock. Highlighting and following the current passage can be changed in **Settings → Appearance**.

## Dictate locally

Place the cursor or select text to replace, then choose **Dictate**. Select a model, language and microphone. The refresh button updates the device list. Microphone access must be allowed by the operating system.

The dictation controls stay centered and adapt to the window width, keeping their full labels visible. The button always reads **Live text**. When enabled, it uses a light blue fill in light mode and a dark blue fill in dark mode; when disabled, its fill is neutral. With Live text enabled, recognition periodically updates provisional words in the editor. **Stop recording** finishes capture and produces the final text from the recording. Provisional words may change. With Live text disabled, recognition begins after recording stops.

**Cancel** restores the text and selection from before dictation. Errors and an empty final result also preserve the original text. Successful insertion can be undone once with Ctrl+Z. Dictation never starts speech playback automatically. Playback and dictation cannot run simultaneously.

Live text depends on available CPU capacity and is not guaranteed to keep up with speech. For larger models or slower computers, turn Live text off and use record-then-recognize. Recognition CPU threads can be adjusted in Settings.

**Audio file…** accepts WAV, MP3 and FLAC, up to one hour and 1 GiB. Recognition inserts the result at the same cursor/selection position. Edit or save the resulting text, or press Speak.

### Microphone level

The bottom status row contains a centered microphone meter when the dictation panel is open. During recording, its bars expand from the center outward with the microphone level. Both halves mirror the same mono input; they are not left and right channels. The meter shows input level, not recognition accuracy.

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

## Saved API keys

Encrypted saving is optional and separate for each server profile. Leave **Save API key encrypted** off to keep a key for the current session only.

To save a key, enter it in **API key**, enable **Save API key encrypted**, and provide a passphrase of at least **12 characters** and confirmation when prompted. Choose **Save** in Settings to commit the encrypted credential. The passphrase is never stored.

After restarting SpeechDesk, the saved key remains locked. Choose **Unlock…** and enter that profile's passphrase, or unlock when prompted to use it. **Lock now** immediately removes only the selected profile's unlocked key from use. It does not delete the encrypted file or affect other profiles. This explicit lock still applies if you later cancel Settings.

A wrong passphrase or a damaged credential file does not overwrite or delete the saved key. There is no passphrase reset or key recovery. Use **Replace saved key…** to enter a valid API key, which can be the same key or a replacement from your provider, and a new passphrase with confirmation. Choose Settings **Save** to replace only that profile's credential.

Turning **Save API key encrypted** off and choosing **Save** removes the profile's stored key. A session-only key can still be used. New saves, replacements, and removals are pending until Settings Save; **Cancel** discards these pending changes. A failed save preserves the previous saved credential.

Profile exports contain no API keys, encrypted credential files, or passphrases. Encrypted files are stored in the `credentials` subdirectory of SpeechDesk's configuration directory. This feature works without an operating-system keychain. Read [Security and privacy](../SECURITY.md) for its protection and limitations.

## Preferences and cleanup

Choose **Settings → About → Manual** to open the [online manual](https://davoda1.github.io/Speech-Desk-Manual/) in your browser.

Settings and installed models use the `Speech-Desk` directory in the user's configuration location. API keys are session-only by default; optional encrypted saved keys remain locked after restart until the profile is unlocked with its passphrase. Model import is independent of the Settings dialog's Save/Cancel buttons: a successfully imported model remains installed.

Temporary playback and recognition files are removed during normal cleanup. A forced process termination can leave operating-system temporary files behind. Keep anything you want to retain by using Save text or Save audio.
