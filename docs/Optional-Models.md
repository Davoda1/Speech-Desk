# Optional dictation models

Two separate packs add higher-capacity Whisper models to local dictation. Each model is distributed as **one complete ZIP**. These optional weights are not included in the normal application or source archives. The four bundled models remain available without installing either pack.

| Pack | Model weights | SHA-256 of weights |
| --- | --- | --- |
| `SpeechDesk-1.0.0-Whisper-Large-v3-Turbo-Expansion.zip` | 574,041,195 bytes | `394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2` |
| `SpeechDesk-1.0.0-Whisper-Large-v3-Expansion.zip` | 1,081,140,203 bytes | `d75795ecff3f83b5faa89d1900604ad8c780abd5739fae406de19f23ecd98ad1` |

## Install

1. Download the complete ZIP for the desired model from the release assets.
2. Open **Settings → Dictation → Import model pack…** and select the ZIP directly.
3. Wait for validation and import, close Settings, then select the model in **Dictate**.

Do not extract the pack before importing it. The same pack works on Windows and Linux, with a separate installation on each computer. Installed models persist across restarts in `%AppData%\Speech-Desk\models` or `$XDG_CONFIG_HOME/Speech-Desk/models` (normally `~/.config/Speech-Desk/models`). The downloaded ZIP can be removed after a successful import.

The importer validates the catalogued model ID, byte count, SHA-256, CRC and archive structure. Cancellation leaves an existing installed model intact, unless the final atomic commit has already completed. Imported models remain installed even if the Settings dialog is subsequently cancelled.

## Performance

Both models run on the local CPU. No dedicated GPU is required. Large v3 Turbo generally aims for lower processing time; full Large v3 provides another accuracy/performance tradeoff. Neither is guaranteed to outperform smaller models on every recording.

Model-file size is not peak RAM use. Allow several gigabytes of free memory and additional disk space for the ZIP, installed model and temporary data. A computer with at least 8 GB total RAM is a planning allowance, not a measured minimum or a real-time guarantee. Start with **Live text off** on modest hardware.

## Pack format and reconstruction

Packs use ZIP's stored method and contain exactly `manifest.json`, `LICENSE.txt`, `README.txt` and the catalogued weights file. The manifest identifies `speechdesk-model-pack`, format version `1`, model ID, filename, byte count and SHA-256. Arbitrary ZIP files or unrelated model downloads are not installable packs. Encrypted entries, symlinks, ZIP64, compressed entries, data descriptors and unsafe paths are rejected.

`vendor/optional-models.json` pins the two model weights. `packaging/create_model_packs.py` constructs complete ZIP packs from verified weights and adds distribution instructions and the model's MIT notice. The release's single `SHA256SUM.txt` records each finished ZIP's hash.

Upstream weights: [ggerganov/whisper.cpp, revision 5359861c739e955e79d9a303bcbc70fb988958b1](https://huggingface.co/ggerganov/whisper.cpp/tree/5359861c739e955e79d9a303bcbc70fb988958b1).
