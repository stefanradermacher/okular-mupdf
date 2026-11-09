# okular-mupdf

An additional Okular backend (generator plugin) for PDF documents that renders
with MuPDF instead of Poppler. It registers for `application/pdf` with
`X-KDE-Priority: 3` and is therefore preferred over Poppler (priority 1).

## Gentoo

The ebuild lives in the overlay https://github.com/stefanradermacher/gentoo-overlay:

    eselect repository add stefanradermacher git https://github.com/stefanradermacher/gentoo-overlay.git
    emerge --sync stefanradermacher
    emerge -av kde-apps/okular-mupdf

## Building and installing manually

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
    sudo cmake --install build        # installs to /usr/lib64/qt6/plugins/okular_generators/

Restart Okular afterwards. The plugin has to match the installed Okular
version (ABI) and must be rebuilt after Okular updates. If it no longer loads,
Okular silently falls back to Poppler.

## Choosing the backend

* By default MuPDF wins because of its higher priority.
* Back to Poppler: set the priority in `libokularGenerator_mupdf.json` below 1
  and rebuild, or remove the plugin.
* Choose per file: Okular, Settings → Configure Okular → General →
  "Ask for the backend when opening files" (`chooseGenerators`).

## Features

Rendering (tiles, zoom, rotation), text extraction/search/selection, links
(internal/URL), table of contents, document info, password prompt.

Not implemented: editing annotations, filling in forms, signatures, printing
through the backend, layers, font info, media. Annotations and form fields are
displayed, though.

## License

GPL-2.0-or-later, see `LICENSE`.
