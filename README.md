# okular-mupdf

Ein zusätzliches Okular-Backend (Generator-Plugin) für PDF-Dokumente, das
MuPDF statt Poppler zum Rendern benutzt. Es meldet sich für `application/pdf`
mit `X-KDE-Priority: 3` an und wird damit vor Poppler (Priorität 1) gewählt.

## Gentoo

Ebuild im Overlay https://github.com/stefanradermacher/gentoo-overlay:

    eselect repository add stefanradermacher git https://github.com/stefanradermacher/gentoo-overlay.git
    emerge --sync stefanradermacher
    emerge -av kde-apps/okular-mupdf

## Manuell bauen und installieren

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
    sudo cmake --install build        # nach /usr/lib64/qt6/plugins/okular_generators/

Okular danach neu starten. Das Plugin muss zur installierten Okular-Version
passen (ABI) und nach Okular-Updates neu gebaut werden.

## Backend wählen

* Standardmäßig gewinnt MuPDF (höhere Priorität).
* Zurück zu Poppler: in `libokularGenerator_mupdf.json` die Priorität unter 1 setzen
  und neu bauen, oder das Plugin entfernen.
* Pro Öffnen wählen: Okular, Einstellungen → Okular einrichten → Allgemein →
  "Beim Öffnen von Dateien nach dem Backend fragen" (`chooseGenerators`).

## Funktionsumfang

Rendern (Kacheln, Zoom, Rotation), Textextraktion/Suche/Markieren, Links
(intern/URL), Inhaltsverzeichnis, Dokumentinfo, Passwortabfrage.
Nicht enthalten: Annotationen bearbeiten, Formulare ausfüllen, Signaturen,
Drucken über das Backend, Ebenen, Schriftinfo, Medien.
