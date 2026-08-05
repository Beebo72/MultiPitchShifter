# MultiPitchShifter the program used in ICF+ and BeeboVideo

`multipitch` is a command-line tool that reads a 16-bit PCM WAV file, applies
multiple pitch offsets, stretches each layer back to the original timing,
mixes the layers, normalizes the result, and writes a new WAV file.

## YOU MUST OBTAIN THE SOURCE CODE FROM THE OFFICAL REPOSITORIES


## Build

The supported build uses Make and a C/C++ compiler:

```sh
make
```

For a clean rebuild:

```sh
make rebuild
```

The build creates the `multipitch` executable. The embedded pitch-shifting
engine requires GCC/G++ 11 or newer; Bungee requires C++20.

## Usage

```sh
./multipitch input.wav output.wav -12,-7,0,4,7,12
```

Pitch values are specified in semitones. `0` keeps the original pitch, `12` is
one octave up, and `-12` is one octave down.

For voice-like audio, enable formant preservation:

```sh
./multipitch input.wav output.wav 4 --preserve-formants
```

Input must be an uncompressed 16-bit PCM WAV file.

## License

This project is licensed under the MIT License. Third-party dependencies under
`import/` retain their own licenses.
