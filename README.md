# Live Autocomplete for Windows

A lightweight C autocomplete tool that displays word suggestions while you type. It combines a dictionary, a personal word-frequency cache, and a bigram/trigram language model to rank suggestions.

## Features

- **Live suggestions:** Displays a popup near the text cursor.
- **Keyboard controls:** Navigate suggestions and accept a completion without leaving the keyboard.
- **Dictionary lookup:** Uses `dictionary.txt` for words and their frequency values.
- **Personal cache:** Learns which words you type often and saves their counts in `cache.txt`.
- **Context-aware prediction:** Uses previous words to predict likely next words through a bigram/trigram model stored in `ngrams.txt`.
- **Persistent learning:** Saves updated cache and N-gram counts when the program exits normally.
- **Light and dark themes:** Follows the Windows app theme by default, with command-line overrides.
- **Training mode:** Can train from a plain-text file before starting the live popup.

## Requirements

- Windows (uses the Win32 API).
- A C compiler:
  - MinGW-w64 GCC, or
  - Microsoft Visual C++ (`cl`).
- The three data files described below. `cache.txt` and `ngrams.txt` can be created or started empty; `dictionary.txt` is required.

## Project files

Keep these files together in the same folder as the compiled executable:

| File | Purpose |
|---|---|
| `autocomplete.c` | Main application source code |
| `dictionary.txt` | Dictionary words and their frequency values |
| `cache.txt` | Counts of words typed by the user; created if missing |
| `ngrams.txt` | Learned context-to-next-word counts |

The program locates its data files relative to the executable, not the current working directory.

## Build

### MinGW-w64 / GCC

Open a terminal in the project folder and run:

```bash
gcc autocomplete.c -o autocomplete.exe -O2 -luser32 -lgdi32 -ladvapi32
```

### Visual Studio Developer Command Prompt

Open a **Developer Command Prompt for Visual Studio**, change to the project folder, and run:

```bat
cl autocomplete.c
```

The source uses MSVC linker pragmas for the required Windows libraries.

## Run

Start the executable from a terminal:

```bat
autocomplete.exe
```

By default, the program is intended to provide suggestions while typing in **Microsoft Word**. Keep the console window open while using it. The console reports whether the dictionary, cache, and N-gram model loaded successfully.

To quit, press **Ctrl+C** in the console. The program attempts to save learned data on exit.

## Keyboard controls

| Key | Action |
|---|---|
| `Up Arrow` | Select the previous suggestion |
| `Down Arrow` | Select the next suggestion |
| `Tab` | Accept the selected suggestion |
| `Esc` | Dismiss the suggestion popup |
| `Ctrl+C` in the console | Quit the program |

The program tracks ordinary editing keys such as Backspace, Delete, Left Arrow, and Right Arrow so suggestions can follow edits to the current word. Mouse clicks reset its tracked typing buffer.

## Command-line options

Options can be combined.

| Option | Description |
|---|---|
| `--any` | Enable suggestions in other applications too; useful for testing in apps such as Notepad |
| `--dark` | Force the dark popup theme |
| `--light` | Force the light popup theme |
| `--nonext` | Disable next-word prediction after a space |
| `--nolearn` | Disable learning and prevent the program from writing updated data files |
| `--order 2` | Use a bigram model |
| `--order 3` | Use a trigram model (default) |
| `--train FILE` | Train from a plain-text file, save the model/cache, and exit |

Examples:

```bat
autocomplete.exe --any
autocomplete.exe --dark --any
autocomplete.exe --order 2
autocomplete.exe --nolearn
autocomplete.exe --train sample.txt
```

For training, use a text file whose words are present in `dictionary.txt`. The trainer counts words and word sequences; punctuation and unknown words break the context.

## How suggestions are ranked

The application combines three sources:

1. **N-gram model:** Estimates which word is likely to follow the previous one or two words.
2. **Personal cache:** Gives weight to words the user types frequently.
3. **Dictionary:** Supplies fallback candidates when learned sources do not fill the suggestion list.

The model stores counts for contexts and their following words. For example, a line such as `i am 60` represents a bigram count, while `thank you very 12` represents a trigram count. The implementation blends context probabilities with shorter-context evidence so predictions can fall back when a longer context has little or no data.

Suggestion labels indicate the strongest available evidence, such as `tri 62%`, `bi 45%`, `typed 12`, or a dictionary frequency value.

## Data-file formats

### `dictionary.txt`

One word and one numeric frequency per line, separated by whitespace:

```text
the 7.73
to 7.43
and 7.41
```

The dictionary loader accepts alphabetic words (`a`–`z`, case-insensitive). Words containing punctuation or digits are skipped.

### `cache.txt`

One word and its typed count per line:

```text
hello 4
world 2
```

The cache is updated as you type. The saved file keeps frequently typed words, with a maximum of 2,000 entries; words with counts below the configured save threshold are not written.

### `ngrams.txt`

Each line stores a context, the next word, and its count:

```text
i am 60
thank you very 12
```

A two-word line followed by a number represents a bigram (`context next-word count`). A three-word sequence followed by a number represents a trigram (`older-word newer-word next-word count`).

## Notes and troubleshooting

- If the program reports that `dictionary.txt` cannot be opened, place it next to `autocomplete.exe`.
- If `ngrams.txt` is missing, the model starts empty and learns as you type.
- If `cache.txt` is missing, the program attempts to create an empty cache file.
- If suggestions do not appear in another application, run with `--any`.
- If the program cannot install its keyboard or mouse hook, check whether Windows or security software is blocking the application.
- `--nolearn` disables learning and data-file writes for that run.
- This program uses low-level Windows input hooks. Use it only on a computer and in applications where you have permission to run it.

## License

No license information was included with the provided source. Add a `LICENSE` file if you intend to distribute the project.
